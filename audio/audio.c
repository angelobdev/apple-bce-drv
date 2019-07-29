#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/random.h>
#include <sound/core.h>
#include <sound/initval.h>
#include "audio.h"

static int aaudio_alsa_index = SNDRV_DEFAULT_IDX1;
static char *aaudio_alsa_id = SNDRV_DEFAULT_STR1;

static dev_t aaudio_chrdev;
static struct class *aaudio_class;

static int aaudio_init_cmd(struct aaudio_device *a);
static int aaudio_init_bs(struct aaudio_device *a);

static int aaudio_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    struct aaudio_device *aaudio = NULL;
    int status = 0;
    u32 cfg;

    pr_info("aaudio: capturing our device\n");

    if (pci_enable_device(dev))
        return -ENODEV;
    if (pci_request_regions(dev, "aaudio")) {
        status = -ENODEV;
        goto fail;
    }

    aaudio = kzalloc(sizeof(struct aaudio_device), GFP_KERNEL);
    if (!aaudio) {
        status = -ENOMEM;
        goto fail;
    }

    aaudio->bce = global_bce;
    if (!aaudio->bce) {
        dev_warn(&dev->dev, "aaudio: No BCE available\n");
        status = -EINVAL;
        goto fail;
    }

    aaudio->pci = dev;
    pci_set_drvdata(dev, aaudio);

    aaudio->devt = aaudio_chrdev;
    aaudio->dev = device_create(aaudio_class, &dev->dev, aaudio->devt, NULL, "aaudio");
    if (IS_ERR_OR_NULL(aaudio->dev)) {
        status = PTR_ERR(aaudio_class);
        goto fail;
    }

    init_completion(&aaudio->remote_alive);

    /* Init: set an unknown flag in the bitset */
    if (pci_read_config_dword(dev, 4, &cfg))
        dev_warn(&dev->dev, "aaudio: pci_read_config_dword fail\n");
    if (pci_write_config_dword(dev, 4, cfg | 6u))
        dev_warn(&dev->dev, "aaudio: pci_write_config_dword fail\n");

    dev_info(aaudio->dev, "aaudio: bs len = %llx\n", pci_resource_len(dev, 0));
    aaudio->reg_mem_bs = pci_iomap(dev, 0, 0);
    aaudio->reg_mem_cfg = pci_iomap(dev, 4, 0);

    aaudio->reg_mem_gpr = (u32 __iomem *) ((u8 __iomem *) aaudio->reg_mem_cfg + 0xC000);

    if (IS_ERR_OR_NULL(aaudio->reg_mem_bs) || IS_ERR_OR_NULL(aaudio->reg_mem_cfg)) {
        dev_warn(&dev->dev, "aaudio: Failed to pci_iomap required regions\n");
        goto fail;
    }

    if (aaudio_bce_init(aaudio)) {
        dev_warn(&dev->dev, "aaudio: Failed to init BCE command transport\n");
        goto fail;
    }

    if (snd_card_new(aaudio->dev, aaudio_alsa_index, aaudio_alsa_id, THIS_MODULE, 0, &aaudio->card)) {
        dev_err(&dev->dev, "aaudio: Failed to create ALSA card\n");
        goto fail;
    }
    strcpy(aaudio->card->driver, "Apple Audio");
    strcpy(aaudio->card->shortname, "Apple T2 Audio");
    strcpy(aaudio->card->longname, "Apple T2 Audio");

    if (aaudio_init_cmd(aaudio)) {
        dev_err(&dev->dev, "aaudio: Failed to initialize over BCE\n");
        goto fail_snd;
    }
    if (aaudio_init_bs(aaudio)) {
        dev_err(&dev->dev, "aaudio: Failed to initialize BufferStruct\n");
        goto fail_snd;
    }

    if (snd_card_register(aaudio->card)) {
        dev_err(&dev->dev, "aaudio: Failed to register ALSA sound device\n");
        goto fail_snd;
    }

    return 0;

fail_snd:
    snd_card_free(aaudio->card);
fail:
    if (aaudio && aaudio->dev)
        device_destroy(aaudio_class, aaudio->devt);
    kfree(aaudio);

    if (!IS_ERR_OR_NULL(aaudio->reg_mem_bs))
        pci_iounmap(dev, aaudio->reg_mem_bs);
    if (!IS_ERR_OR_NULL(aaudio->reg_mem_cfg))
        pci_iounmap(dev, aaudio->reg_mem_cfg);

    pci_release_regions(dev);
    pci_disable_device(dev);

    if (!status)
        status = -EINVAL;
    return status;
}

static void aaudio_remove(struct pci_dev *dev)
{
    struct aaudio_device *aaudio = pci_get_drvdata(dev);

    snd_card_free(aaudio->card);
    pci_iounmap(dev, aaudio->reg_mem_bs);
    pci_iounmap(dev, aaudio->reg_mem_cfg);
    device_destroy(aaudio_class, aaudio->devt);
    pci_free_irq_vectors(dev);
    pci_release_regions(dev);
    pci_disable_device(dev);
    kfree(aaudio);
}

static int aaudio_init_cmd(struct aaudio_device *a)
{
    int status;
    struct aaudio_send_ctx sctx;
    struct aaudio_msg buf;
    u64 dev_cnt, dev_i;
    aaudio_device_id_t *dev_l;

    if ((status = aaudio_send(a, &sctx, 500,
                              aaudio_msg_write_alive_notification, 1, 3))) {
        dev_err(a->dev, "Sending alive notification failed\n");
        return status;
    }

    if (wait_for_completion_timeout(&a->remote_alive, msecs_to_jiffies(500)) == 0) {
        dev_err(a->dev, "Timed out waiting for remote\n");
        return -ETIMEDOUT;
    }
    dev_info(a->dev, "Continuing init\n");

    buf = aaudio_reply_alloc();
    if ((status = aaudio_cmd_get_device_list(a, &buf, &dev_l, &dev_cnt))) {
        dev_err(a->dev, "Failed to get device list\n");
        aaudio_reply_free(&buf);
        return status;
    }
    for (dev_i = 0; dev_i < dev_cnt; ++dev_i) {
        dev_info(a->dev, "Remote device: %llx\n", dev_l[dev_i]);
    }
    aaudio_reply_free(&buf);

    if ((status = aaudio_cmd_set_remote_access(a, AAUDIO_REMOTE_ACCESS_ON))) {
        dev_err(a->dev, "Failed to set remote access\n");
        return status;
    }

    return 0;
}

static int aaudio_init_bs(struct aaudio_device *a)
{
    int i, j;
    u32 ver, sig, bs_base;

    ver = ioread32(&a->reg_mem_gpr[0]);
    if (ver < 3) {
        dev_err(a->dev, "aaudio: Bad GPR version (%u)", ver);
        return -EINVAL;
    }
    sig = ioread32(&a->reg_mem_gpr[1]);
    if (sig != AAUDIO_SIG) {
        dev_err(a->dev, "aaudio: Bad GPR sig (%x)", sig);
        return -EINVAL;
    }
    bs_base = ioread32(&a->reg_mem_gpr[2]);
    a->bs = (struct aaudio_buffer_struct *) ((u8 *) a->reg_mem_bs + bs_base);
    if (a->bs->signature != AAUDIO_SIG) {
        dev_err(a->dev, "aaudio: Bad BS sig (%x)", a->bs->signature);
        return -EINVAL;
    }
    dev_info(a->dev, "aaudio: BS ver = %i\n", a->bs->version);
    dev_info(a->dev, "aaudio: Num devices = %i\n", a->bs->num_devices);
    for (i = 0; i < a->bs->num_devices; i++) {
        dev_info(a->dev, "aaudio: Device %i %s\n", i, a->bs->devices[i].name);
        for (j = 0; j < a->bs->devices[i].num_input_streams; j++) {
            dev_info(a->dev, "aaudio: Device %i Stream %i: Input; Buffer Count = %i\n", i, j,
                    a->bs->devices[i].input_streams[j].num_buffers);
        }
        for (j = 0; j < a->bs->devices[i].num_output_streams; j++) {
            dev_info(a->dev, "aaudio: Device %i Stream %i: Output; Buffer Count = %i\n", i, j,
                     a->bs->devices[i].output_streams[j].num_buffers);
        }
    }

    return 0;
}

void aaudio_handle_notification(struct aaudio_device *a, struct aaudio_msg *msg)
{
    struct aaudio_send_ctx sctx;
    struct aaudio_msg_base base;
    if (aaudio_msg_read_base(msg, &base))
        return;
    switch (base.msg) {
        case AAUDIO_MSG_NOTIFICATION_BOOT:
            dev_info(a->dev, "Received boot notification from remote\n");

            /* Resend the alive notify */
            if (aaudio_send(a, &sctx, 500,
                    aaudio_msg_write_alive_notification, 1, 3)) {
                pr_err("Sending alive notification failed\n");
            }
            break;
        case AAUDIO_MSG_NOTIFICATION_ALIVE:
            dev_info(a->dev, "Received alive notification from remote\n");
            complete_all(&a->remote_alive);
            break;
        default:
            dev_info(a->dev, "Unhandled notification %i", base.msg);
            break;
    }
}

#define aaudio_send_cmd_response(a, sctx, msg, fn, ...) \
    if (aaudio_send_with_tag(a, sctx, ((struct aaudio_msg_header *) msg->data)->tag, 500, fn, ##__VA_ARGS__)) \
        pr_err("aaudio: Failed to reply to a command\n");

void aaudio_handle_cmd_timestamp(struct aaudio_device *a, struct aaudio_msg *msg)
{
    struct aaudio_send_ctx sctx;
    u64 devid, timestamp, update_seed;
    aaudio_msg_read_update_timestamp(msg, &devid, &timestamp, &update_seed);
    dev_info(a->dev, "Received timestamp update for dev=%llx ts=%llx seed=%llx\n", devid, timestamp, update_seed);

    aaudio_send_cmd_response(a, &sctx, msg,
            aaudio_msg_write_update_timestamp_response);
}

void aaudio_handle_command(struct aaudio_device *a, struct aaudio_msg *msg)
{
    struct aaudio_msg_base base;
    if (aaudio_msg_read_base(msg, &base))
        return;
    switch (base.msg) {
        case AAUDIO_MSG_UPDATE_TIMESTAMP:
            aaudio_handle_cmd_timestamp(a, msg);
            break;
        default:
            dev_info(a->dev, "Unhandled device command %i", base.msg);
            break;
    }
}

static struct pci_device_id aaudio_ids[  ] = {
        { PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1803) },
        { 0, },
};

struct pci_driver aaudio_pci_driver = {
        .name = "aaudio",
        .id_table = aaudio_ids,
        .probe = aaudio_probe,
        .remove = aaudio_remove
};


int aaudio_module_init(void)
{
    int result;
    if ((result = alloc_chrdev_region(&aaudio_chrdev, 0, 1, "aaudio")))
        goto fail_chrdev;
    aaudio_class = class_create(THIS_MODULE, "aaudio");
    if (IS_ERR(aaudio_class)) {
        result = PTR_ERR(aaudio_class);
        goto fail_class;
    }
    
    result = pci_register_driver(&aaudio_pci_driver);
    if (result)
        goto fail_drv;
    return 0;

fail_drv:
    pci_unregister_driver(&aaudio_pci_driver);
fail_class:
    class_destroy(aaudio_class);
fail_chrdev:
    unregister_chrdev_region(aaudio_chrdev, 1);
    if (!result)
        result = -EINVAL;
    return result;
}

void aaudio_module_exit(void)
{
    pci_unregister_driver(&aaudio_pci_driver);
    class_destroy(aaudio_class);
    unregister_chrdev_region(aaudio_chrdev, 1);
}

module_param_named(index, aaudio_alsa_index, int, 0444);
MODULE_PARM_DESC(index, "Index value for Apple Internal Audio soundcard.");
module_param_named(id, aaudio_alsa_id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for Apple Internal Audio soundcard.");