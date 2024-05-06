#include <linux/module.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>
#define GRANULARITY	20000000 //20ms
#define HORIZON	100000000 //100ms
// #define HORIZON	20000000 //20ms
#define TW_LENGTH	100

// #define TARGET_RATE 1000000 //1 mbit
#define TARGET_RATE 100000 //100 kbit
// #define TARGET_RATE 1000 //1 kbit
const u64 TARGET_RATE_NS = (1000000000/TARGET_RATE);  
// #define MC_DEBUG

DEFINE_PER_CPU(u64,last_time_span);
DEFINE_PER_CPU(u64,mean_time_span);
DEFINE_PER_CPU(struct sk_buff *, next_pkt);
DEFINE_PER_CPU(struct list_head[TW_LENGTH], timing_wheel);
struct mc_sched_data {
	struct list_head timing_wheel[TW_LENGTH];
	u64 last_enqueued;
};

static int mc_qdisc_enqueue(struct sk_buff *skb, struct Qdisc *sch,
                struct sk_buff **to_free)
{
	struct mc_sched_data *priv = qdisc_priv(sch);
	int ret =  NET_XMIT_SUCCESS;
	u32 len = qdisc_pkt_len(skb);
	u64 now = ktime_get_ns();
	if (priv->last_enqueued == 0 || priv->last_enqueued + HORIZON < now)
		skb->skb_mstamp_ns = now;
	else
		skb->skb_mstamp_ns = priv->last_enqueued+len*8*TARGET_RATE_NS; 

	priv->last_enqueued = skb->skb_mstamp_ns;
	list_add_tail(&skb->list, &priv->timing_wheel[0]);
#ifdef MC_DEBUG
	pr_err("Enqueue packet on %d\tprotocol %x\tpkt_ts: %llu\tts: %llu\n", smp_processor_id(), skb->protocol, skb->skb_mstamp_ns, ktime_get_ns());
#endif
	return ret;
}

static struct sk_buff *mc_qdisc_dequeue(struct Qdisc *sch)
{
	struct sk_buff *s;
	struct mc_sched_data *priv = qdisc_priv(sch);
	u64 now = ktime_get_ns();
#ifdef MC_DEBUG
	u64 *old = &per_cpu(last_time_span, smp_processor_id());

	if (*old == 0)
		*old = ktime_get_ns();
	else {
		u64 now = ktime_get_ns();
		u64 time_span = now-*old;	
		u64 *mean = &per_cpu(mean_time_span, smp_processor_id());
		*mean = (unsigned long long)((*mean >> 1) + (*mean >> 2) + (*mean >> 3)+(time_span>>3));
		*old = now;
		pr_err("time_span: %llu\tmean: %llu\tcpu: %d\n", time_span, *mean, smp_processor_id());
	}
#endif

	if (list_empty(&priv->timing_wheel[0]))	{
		return NULL;
	}

	s = list_first_entry(&priv->timing_wheel[0], struct sk_buff, list);
	if (now >= s->skb_mstamp_ns)	
		list_del(&s->list);
	else
		return NULL;

#ifdef MC_DEBUG
	pr_err("Sending skb from %d protocol: %x\n", smp_processor_id(), s->protocol);
#endif
	return s;
}


static int mc_init(struct Qdisc *sch, struct nlattr *opt,
        struct netlink_ext_ack *extack)
{
	int i;
	u64 *t;
	struct mc_sched_data *priv = qdisc_priv(sch);
    pr_err("In: %s v0.5\n", __func__);
	for_each_possible_cpu(i) {
		t = &per_cpu(last_time_span, i);
		*t = 0;
		t = &per_cpu(mean_time_span, i);
		*t=0;
		for(int j = 0; j < TW_LENGTH; j++)
			INIT_LIST_HEAD(&(priv->timing_wheel[j]));
	}
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
	.priv_size = sizeof(struct mc_sched_data),
    .dump   = mc_dump,
    .owner  = THIS_MODULE,
};

static int __init mc_module_init(void)
{
    return  register_qdisc(&mc_qdisc_ops);
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
