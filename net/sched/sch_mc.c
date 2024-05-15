#include <linux/module.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>
#include <net/sock.h>

#define GRANULARITY	20000000 //20ms
// #define HORIZON	1000000000 //1s
#define HORIZON	20000000 //20ms
#define TW_LENGTH	1

// #define TARGET_RATE 10000000 //10 mbit
// #define TARGET_RATE 1000000 //1 mbit
#define TARGET_RATE 500000 //500 kbit
// #define TARGET_RATE 100000 //100 kbit
// #define TARGET_RATE 1000 //1 kbit
const u64 TARGET_RATE_NS = (NSEC_PER_SEC/TARGET_RATE);  
// #define MC_DEBUG

DEFINE_PER_CPU(u64,last_time_span);
DEFINE_PER_CPU(u64,mean_time_span);
DEFINE_PER_CPU(struct sk_buff *, next_pkt);
DEFINE_PER_CPU(struct list_head[TW_LENGTH], timing_wheel);
static bool *active;
static int num_tx_queues;
static int counter;
struct mc_sched_data {
	struct list_head timing_wheel[TW_LENGTH];
	u64 last_enqueued;
	u64 qlen;
	u32 txq_num;
	struct qdisc_watchdog watchdog;
};

static int mc_qdisc_enqueue(struct sk_buff *skb, struct Qdisc *sch,
                struct sk_buff **to_free)
{
	struct mc_sched_data *priv = qdisc_priv(sch);
	int ret =  NET_XMIT_SUCCESS;
	u32 len = qdisc_pkt_len(skb);
	u64 now = ktime_get_ns();
	int num_active_qs = 0;

	int i;

	if (priv->qlen > 100) {
		qdisc_drop(skb, sch, to_free);
		return NET_XMIT_DROP;
	}


	// pr_err("cpu %d, sch: %lx,tx queue %u", smp_processor_id(), (unsigned long)sch, priv->txq_num);
	WRITE_ONCE(active[priv->txq_num], true);
	for(i = 0; i < num_tx_queues; ++i){
		if (READ_ONCE(active[i]) == true)
			num_active_qs++;
	}
	skb->skb_mstamp_ns = priv->last_enqueued+len*8*TARGET_RATE_NS*num_active_qs; 
	if (priv->last_enqueued == 0 || skb->skb_mstamp_ns < now){
		skb->skb_mstamp_ns = now;
#ifdef MC_DEBUG
		pr_err("set to now\n");
#endif
	}

	if (counter++ % 10==0)
		pr_err("Active queues: %d\n", num_active_qs);

	priv->last_enqueued = skb->skb_mstamp_ns;
	list_add_tail(&skb->list, &priv->timing_wheel[0]);
	priv->qlen++;
	sch->q.qlen++;
#ifdef MC_DEBUG
	pr_err("Enqueue: on cpu %d: txq: %u num_active_cpus %d, qlen: %llu now: %llu skbts: %llu pktlen: %u\n",
			smp_processor_id(),priv->txq_num, num_active_qs, priv->qlen, now, skb->skb_mstamp_ns, len);
#endif
	return ret;
}

static struct sk_buff *mc_qdisc_dequeue(struct Qdisc *sch)
{
	struct sk_buff *s;
	struct mc_sched_data *priv = qdisc_priv(sch);
	u64 now = ktime_get_ns();
#ifdef MC_DEBUG
	pr_err("Try to dequeue from txq %u at %llu\n", priv->txq_num, now);
	// u64 *old = &per_cpu(last_time_span, smp_processor_id());
	//
	// if (*old == 0)
	// 	*old = ktime_get_ns();
	// else {
	// 	u64 now = ktime_get_ns();
	// 	u64 time_span = now-*old;	
	// 	u64 *mean = &per_cpu(mean_time_span, smp_processor_id());
	// 	*mean = (unsigned long long)((*mean >> 1) + (*mean >> 2) + (*mean >> 3)+(time_span>>3));
	// 	*old = now;
	// 	pr_err("time_span: %llu\tmean: %llu\tcpu: %d\n", time_span, *mean, smp_processor_id());
	// }
#endif

	if (list_empty(&priv->timing_wheel[0]))	{
		WRITE_ONCE(active[priv->txq_num], false);
		return NULL;
	}

	s = list_first_entry(&priv->timing_wheel[0], struct sk_buff, list);
	if (s->skb_mstamp_ns <= now+HORIZON){
		list_del(&s->list);
		priv->qlen--;
		sch->q.qlen++;
#ifdef MC_DEBUG
		pr_err("Dequeue: on cpu %d of txq %u: qlen %llu ts: %llu skbts: %llu pktlen: %u\n", smp_processor_id(), priv->txq_num, priv->qlen, now, s->skb_mstamp_ns, qdisc_pkt_len(s));
#endif
	}
	else {
		qdisc_watchdog_schedule_range_ns(&priv->watchdog, s->skb_mstamp_ns, 10 * NSEC_PER_USEC /*timer slack*/);
		return NULL;
	}

	return s;
}


static int mc_init(struct Qdisc *sch, struct nlattr *opt,
        struct netlink_ext_ack *extack)
{
	int i;
	u64 *t;
	struct mc_sched_data *priv = qdisc_priv(sch);
    pr_err("In: %s v0.7 %lx\n", __func__, (unsigned long)sch);
	for_each_possible_cpu(i) {
		t = &per_cpu(last_time_span, i);
		*t = 0;
		t = &per_cpu(mean_time_span, i);
		*t=0;
		for(int j = 0; j < TW_LENGTH; j++)
			INIT_LIST_HEAD(&(priv->timing_wheel[j]));
	}
	priv->txq_num = num_tx_queues++;
	priv->last_enqueued = 0;
	if (!active)
		active = kzalloc(sch->dev_queue->dev->num_tx_queues, GFP_KERNEL);
	qdisc_watchdog_init_clockid(&priv->watchdog, sch, CLOCK_MONOTONIC);
    return 0;
}

static int mc_dump(struct Qdisc *sch, struct sk_buff *skb)
{
    return 0;
}

static void mc_destroy(struct Qdisc *sch) 
{
	struct mc_sched_data *priv = qdisc_priv(sch);
	qdisc_watchdog_cancel(&priv->watchdog);
	kfree(active);	
}

static struct Qdisc_ops mc_qdisc_ops __read_mostly = {
    .id     = "mc",
    .enqueue = mc_qdisc_enqueue,
    .dequeue = mc_qdisc_dequeue,
	.peek		= mc_qdisc_dequeue,
    .init   = mc_init,
	.destroy = mc_destroy,
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
