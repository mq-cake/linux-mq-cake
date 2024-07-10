#include "linux/math64.h"
#include "linux/netlink.h"
#include "linux/pkt_sched.h"
#include "linux/printk.h"
#include "net/netlink.h"
#include <linux/module.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>
#include <net/sock.h>

#define GRANULARITY	20000000 //20ms
// #define HORIZON	1000000000 //1s
// #define HORIZON	20000000 //20ms
// #define HORIZON	400000 //400us
// #define HORIZON	300000 //300us
// #define HORIZON	200000 //200us
// #define HORIZON	1000 //1us
// #define HORIZON	20000 //20us
// #define HORIZON	100 //200ns
// #define HORIZON	0
#define TW_LENGTH	1

// #define TARGET_RATE 3000000000 //3 gbit
// #define TARGET_RATE 1000000000 //1 gbit
// #define TARGET_RATE	125000000 //(1Bgit/8)
#define TARGET_RATE 500000000/8 //500 mbit
// #define TARGET_RATE 200000000 //200 mbit
// #define TARGET_RATE 100000000 //100 mbit
// #define TARGET_RATE 50000000 //50 mbit
// #define TARGET_RATE 10000000 //10 mbit
// #define TARGET_RATE 1000000 //1 mbit
// #define TARGET_RATE 500000 //500 kbit
// #define TARGET_RATE 100000 //100 kbit
// #define TARGET_RATE 1000 //1 kbit
const u64 TARGET_RATE_NS = (NSEC_PER_SEC/TARGET_RATE);  
// #define MC_DEBUG

DEFINE_PER_CPU(struct sk_buff *, next_pkt);
DEFINE_PER_CPU(struct list_head[TW_LENGTH], timing_wheel);
static bool *active;
static int num_tx_queues;
#ifdef MC_DEBUG
static int counter;
#endif
struct mc_sched_data {
	struct list_head timing_wheel[TW_LENGTH];
	u64 last_enqueued;
	u64 last_dequeued;
	u64 qlen;
	u32 txq_num;
	u16 last_active_queues;
	struct qdisc_watchdog watchdog;
	u64 current_rate;
	u64 time_next_packet;
	u64 max_rate;
	struct list_head mc_list;
};

static int mc_qdisc_enqueue(struct sk_buff *skb, struct Qdisc *sch,
                struct sk_buff **to_free)
{
	struct mc_sched_data *priv = qdisc_priv(sch);
	int ret =  NET_XMIT_SUCCESS;

	// if (priv->qlen > 10000) {
	// 	qdisc_drop(skb, sch, to_free);
	// 	return NET_XMIT_DROP;
	// }


	list_add_tail(&skb->list, &priv->timing_wheel[0]);
	priv->qlen++;
	sch->q.qlen++;

	if (priv->qlen > 10)
		WRITE_ONCE(active[priv->txq_num], true);

	return ret;
}

// old*0.875+new*0.125
#define MOV_AVG(old, tdiff, len) ((old>>1)+(old>>2)+(old>>3) + (div64_u64(len*NSEC_PER_SEC,tdiff)>>3))
static struct sk_buff *mc_qdisc_dequeue(struct Qdisc *sch)
{
	struct sk_buff *s;
	// struct sk_buff *peek_skb;
	// u64 time_to_send;
	int i;
	int num_active_qs = 0;
	u64 now;
	struct mc_sched_data *priv = qdisc_priv(sch);
	u64 len;

	if (list_empty(&priv->timing_wheel[0]))	{
		WRITE_ONCE(active[priv->txq_num], false);
		return NULL;
	}

	/*send immediately*/
	if (priv->max_rate == 0) {
		// pr_err("send packet");
		s = list_first_entry(&priv->timing_wheel[0], struct sk_buff, list);
		list_del(&s->list);
		return s;
	}
		

	for(i = 0; i < num_tx_queues; ++i){
		if (READ_ONCE(active[i]) == true)
			num_active_qs++;
	}

	if (num_active_qs == 0)
		num_active_qs = 1;

#ifdef MC_DEBUG
	if (counter % 100==0)
		pr_err("Active queues: %d\n", num_active_qs);
#endif

	do {
		s = list_first_entry(&priv->timing_wheel[0], struct sk_buff, list);

		//TODO: if flow is regulated
		now = ktime_get_ns();

		if (priv->time_next_packet <= now) {
			priv->qlen--;
			sch->q.qlen--;
			list_del(&s->list);

			len = qdisc_pkt_len(s)*NSEC_PER_SEC*num_active_qs;
			len = div64_ul(len, priv->max_rate);

			if (priv->time_next_packet)
				len -= min(len/2, now-priv->time_next_packet);
#ifdef MC_DEBUG
	if (counter % 100==0)
		pr_err("len : %llu\n", len);
#endif

			priv->time_next_packet = now+len;
		}
		else {
#ifdef MC_DEBUG
			if (counter++ % 100==0)
				pr_err("defer\n");
#endif
			qdisc_watchdog_schedule_ns(&priv->watchdog, priv->time_next_packet/*, 10 * NSEC_PER_USEC */);
			s = NULL;
		}
	} while(0);

	return s;
}

static int mc_dump(struct Qdisc *sch, struct sk_buff *skb)
{
    return 0;
}

static const struct nla_policy mc_policy[TCA_MC_MAX + 1] = {
	[TCA_MC_MAX_RATE]		= { .type = NLA_U32 },
};

static int mc_change(struct Qdisc *sch, struct nlattr *opt,
		struct netlink_ext_ack *extack)
{
	struct mc_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_MC_MAX+1];
	int err = 0;
	pr_err("In %s", __func__);

	err = nla_parse_nested_deprecated(tb, TCA_MC_MAX, opt, mc_policy, NULL);
	if (err < 0) {
		pr_err("Is error");
		return err;
	}

	sch_tree_lock(sch);

	if (tb[TCA_MC_MAX_RATE]) {
		u32 rate = nla_get_u32(tb[TCA_MC_MAX_RATE]);
		pr_err("rate: %u\n", rate);

		q->max_rate = (rate == ~0U) ? ~0UL : rate;
	}

	sch_tree_unlock(sch);
	return err;
}

static void mc_destroy(struct Qdisc *sch) 
{
	struct mc_sched_data *priv = qdisc_priv(sch);
	spinlock_t *root_lock;

	root_lock = qdisc_lock(qdisc_root(sch));
	spin_lock(root_lock);
	list_del_rcu(&priv->mc_list);
	spin_unlock(root_lock);

	qdisc_watchdog_cancel(&priv->watchdog);

	if (active != NULL) {
		kfree(active);	
		active = NULL;
	}
}

static struct Qdisc_ops mc_qdisc_ops;

static int mc_init(struct Qdisc *sch, struct nlattr *opt,
        struct netlink_ext_ack *extack)
{
	int i,err = 0;
	struct net_device *net;
	spinlock_t *root_lock;
	struct mc_sched_data *priv = qdisc_priv(sch);
	struct list_head *pos;
	struct mc_sched_data *lpriv;
    pr_err("In: %s v0.6 %lx\n", __func__, (unsigned long)sch);
	for_each_possible_cpu(i) {
		for(int j = 0; j < TW_LENGTH; j++)
			INIT_LIST_HEAD(&(priv->timing_wheel[j]));
	}
	priv->txq_num = num_tx_queues++;
	priv->last_enqueued = 0;
	priv->max_rate = 0;
	if (!active)
		active = kzalloc(sch->dev_queue->dev->num_tx_queues, GFP_KERNEL);
	qdisc_watchdog_init_clockid(&priv->watchdog, sch, CLOCK_MONOTONIC);

	INIT_LIST_HEAD_RCU(&priv->mc_list);
	root_lock = qdisc_lock(qdisc_root(sch));
	spin_lock(root_lock);
	net = qdisc_dev(sch);
	for (i = 0; i < net->num_tx_queues; ++i){
		if (net->_tx[i].qdisc->ops == &mc_qdisc_ops && net->_tx[i].qdisc->handle != sch->handle) {
			struct mc_sched_data *other_priv = qdisc_priv(net->_tx[i].qdisc);
			list_add_rcu(&priv->mc_list, &other_priv->mc_list);
			break;
		}
	}
	spin_unlock(root_lock);
	list_for_each_rcu(pos, &priv->mc_list) {
		struct mc_sched_data *priv = container_of(pos, struct mc_sched_data, mc_list);
		pr_err("list: %u", priv->txq_num);
	}
	lpriv = container_of(pos, struct mc_sched_data, mc_list);
	pr_err("list: %u", lpriv->txq_num);

	if (opt)
		err = mc_change(sch, opt, extack);
	pr_err("rate: %llu", priv->max_rate);

    return err;
}

static struct Qdisc_ops mc_qdisc_ops __read_mostly = {
    .id			= "mc",
    .enqueue	= mc_qdisc_enqueue,
    .dequeue	= mc_qdisc_dequeue,
	.peek		= mc_qdisc_dequeue,
    .init		= mc_init,
	.change		= mc_change,
	.destroy	= mc_destroy,
	.priv_size	= sizeof(struct mc_sched_data),
    .dump		= mc_dump,
    .owner		= THIS_MODULE,
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
