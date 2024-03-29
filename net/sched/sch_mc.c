#include <linux/module.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>
static int mc_qdisc_enqueue(struct sk_buff *skb, struct Qdisc *sch,
                struct sk_buff **to_free)
{
    pr_err("In: %s\n", __func__);
	qdisc_drop(skb, sch, to_free);
	return NET_XMIT_SUCCESS | __NET_XMIT_BYPASS;
}

static struct sk_buff *mc_qdisc_dequeue(struct Qdisc *sch)
{
    pr_err("In: %s\n", __func__);
    return NULL;
}

static int mc_init(struct Qdisc *sch, struct nlattr *opt,
        struct netlink_ext_ack *extack)
{
    pr_err("In: %s\n", __func__);
    return 0;
}

static int mc_dump(struct Qdisc *sch, struct sk_buff *skb)
{
    return 0;
}

static struct Qdisc_ops mc_qdisc_ops __read_mostly = {
    .id     = "mc",
    .enqueue = mc_qdisc_enqueue,
    .dequeue = mc_qdisc_dequeue,
	.peek		= mc_qdisc_dequeue,
    .init   = mc_init,
    .dump   = mc_dump,
    .owner  = THIS_MODULE,
};

static int __init mc_module_init(void)
{
    int ret;
    ret = register_qdisc(&mc_qdisc_ops);
    pr_err("In: %s, ret: %d\n", __func__, ret);
    return ret ;
}

static void __exit mc_module_exit(void)
{
    unregister_qdisc(&mc_qdisc_ops);
}
module_init(mc_module_init)
module_exit(mc_module_exit)

MODULE_DESCRIPTION("Multicore Traffic Shaper");
MODULE_AUTHOR("Jonas Köppeler");
MODULE_LICENSE("Dual BSD/GPL");
