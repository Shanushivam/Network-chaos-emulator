#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/atomic.h>

#define DEVICE_NAME "chaos_emulator"
#define CLASS_NAME "chaos_class"

#define CHAOS_MAGIC 'C'
#define CHAOS_SET_CONFIG _IOW(CHAOS_MAGIC, 1, struct chaos_config)
#define CHAOS_GET_STATS  _IOR(CHAOS_MAGIC, 2, struct chaos_stats)

struct chaos_config {
    int latency_ms;      
    int loss_rate;       
    int corrupt_rate;    
    int enable_ingress;  
    int enable_egress;   
};

struct chaos_stats {
    unsigned long total_packets;
    unsigned long dropped_packets;
    unsigned long delayed_packets;
    unsigned long corrupted_packets;
};

static dev_t dev_num;
static struct cdev chaos_cdev;
static struct class *chaos_class = NULL;
static struct device *chaos_device = NULL;

static struct chaos_config current_config = {0, 0, 0, 1, 1};
static atomic64_t stat_total = ATOMIC64_INIT(0);
static atomic64_t stat_dropped = ATOMIC64_INIT(0);
static atomic64_t stat_delayed = ATOMIC64_INIT(0);
static atomic64_t stat_corrupted = ATOMIC64_INIT(0);

static struct nf_hook_ops nfho_ingress;
static struct nf_hook_ops nfho_egress;

static unsigned int process_packet(struct sk_buff *skb) {
    unsigned int rand_val;
    atomic64_inc(&stat_total);

    if (!skb) return NF_ACCEPT;

    // 1. Packet Loss Check
    if (current_config.loss_rate > 0) {
        get_random_bytes(&rand_val, sizeof(rand_val));
        if ((rand_val % 100) < current_config.loss_rate) {
            atomic64_inc(&stat_dropped);
            return NF_DROP;
        }
    }

    // 2. Packet Corruption Check (Corrupt Payload Byte)
    if (current_config.corrupt_rate > 0) {
        get_random_bytes(&rand_val, sizeof(rand_val));
        if ((rand_val % 100) < current_config.corrupt_rate) {
            if (skb_try_make_writable(skb, skb->len) == 0) {
                unsigned char *data = skb_network_header(skb);
                if (data && skb->len > 20) {
                    data[19] ^= 0xFF; // Mutate payload byte
                    atomic64_inc(&stat_corrupted);
                }
            }
        }
    }

    // 3. Latency Injection (Capped to prevent Queue Accumulation)
    if (current_config.latency_ms > 0) {
        atomic64_inc(&stat_delayed);
        if (current_config.latency_ms > 50) {
            mdelay(50); // Prevent kernel thread stacking
        } else {
            mdelay(current_config.latency_ms);
        }
    }

    return NF_ACCEPT;
}

static unsigned int hook_ingress(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    if (!current_config.enable_ingress) return NF_ACCEPT;
    return process_packet(skb);
}

static unsigned int hook_egress(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    if (!current_config.enable_egress) return NF_ACCEPT;
    return process_packet(skb);
}

static long chaos_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    switch (cmd) {
        case CHAOS_SET_CONFIG: {
            struct chaos_config new_cfg;
            if (copy_from_user(&new_cfg, (struct chaos_config __user *)arg, sizeof(new_cfg)))
                return -EFAULT;
            current_config = new_cfg;
            break;
        }
        case CHAOS_GET_STATS: {
            struct chaos_stats st;
            st.total_packets = atomic64_read(&stat_total);
            st.dropped_packets = atomic64_read(&stat_dropped);
            st.delayed_packets = atomic64_read(&stat_delayed);
            st.corrupted_packets = atomic64_read(&stat_corrupted);
            if (copy_to_user((struct chaos_stats __user *)arg, &st, sizeof(st)))
                return -EFAULT;
            break;
        }
        default:
            return -ENOTTY;
    }
    return 0;
}

static int chaos_dev_open(struct inode *inodep, struct file *filep) { return 0; }
static int chaos_dev_release(struct inode *inodep, struct file *filep) { return 0; }

static struct file_operations fops = {
    .open = chaos_dev_open,
    .release = chaos_dev_release,
    .unlocked_ioctl = chaos_dev_ioctl,
};

static int __init chaos_init(void) {
    int ret;
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) return ret;

    cdev_init(&chaos_cdev, &fops);
    chaos_cdev.owner = THIS_MODULE;
    ret = cdev_add(&chaos_cdev, dev_num, 1);
    if (ret < 0) goto unregister_chrdev;

    chaos_class = class_create(CLASS_NAME);
    if (IS_ERR(chaos_class)) { ret = PTR_ERR(chaos_class); goto del_cdev; }

    chaos_device = device_create(chaos_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(chaos_device)) { ret = PTR_ERR(chaos_device); goto destroy_class; }

    nfho_ingress.hook = hook_ingress;
    nfho_ingress.hooknum = NF_INET_PRE_ROUTING;
    nfho_ingress.pf = PF_INET;
    nfho_ingress.priority = NF_IP_PRI_FIRST;
    nf_register_net_hook(&init_net, &nfho_ingress);

    nfho_egress.hook = hook_egress;
    nfho_egress.hooknum = NF_INET_POST_ROUTING;
    nfho_egress.pf = PF_INET;
    nfho_egress.priority = NF_IP_PRI_FIRST;
    nf_register_net_hook(&init_net, &nfho_egress);

    return 0;

destroy_class:
    class_destroy(chaos_class);
del_cdev:
    cdev_del(&chaos_cdev);
unregister_chrdev:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

static void __exit chaos_exit(void) {
    nf_unregister_net_hook(&init_net, &nfho_ingress);
    nf_unregister_net_hook(&init_net, &nfho_egress);
    device_destroy(chaos_class, dev_num);
    class_destroy(chaos_class);
    cdev_del(&chaos_cdev);
    unregister_chrdev_region(dev_num, 1);
}

module_init(chaos_init);
module_exit(chaos_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Network Latency and Packet-Loss Chaos Emulator Driver");
