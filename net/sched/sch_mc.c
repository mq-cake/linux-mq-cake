#include "linux/math64.h"
#include "linux/netlink.h"
#include "linux/pkt_sched.h"
#include "linux/printk.h"
#include "net/netlink.h"
#include <linux/module.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>
#include <net/sock.h>

// overlimit: nr_of_watchdog triggers
// qstat.backlog how often ran the queue empty
// qstat.requeues: nr_of_sync loops 

#define SYNC_PERIOD 100*1000*1000 // 100 ms
#define QLEN_LIMIT_DEFAULT 100
#define WD_SLACK 0
 
struct mc_sched_data {
	struct list_head q;		/* queue where the packets are stored */
	u64 qlen;				/* number of enqueued packets */
	u32 txq_num;			/* identifier */
	u64 time_next_packet;	/* time when the next packet should be sent */
	u32 max_rate;			/* maximum transmission rate */
	bool active;			/* flag if this queue is active */
	u16 last_active_qs;		/* how many queues were active the last time we checked */
	u64 last_checked_active;/* timestamp when the last sync happened */
	u64 packets_sent;		/* counter of packets sent by this qdisc */
	u64 sync_time;		/* time period to update the nr of active q's */
	u64 packets_dropped;
	u64 total_nr_of_active_qs;
	u64 nr_of_sync_loops;
	u32 num_tx_queues;		/* the number of other sch_mc queues on the same netdev*/
	u32 qlen_limit;
	u32 wd_slack;
	u64 qdisc_wd_active[64];
	struct list_head mc_list;
	struct qdisc_watchdog watchdog;
	u64 last_enqueued;
	u64 last_dequeued;
	u64 last_watchdog;
};

static int mc_qdisc_enqueue(struct sk_buff *skb, struct Qdisc *sch,
                struct sk_buff **to_free)
{
	struct mc_sched_data *priv = qdisc_priv(sch);
	int ret =  NET_XMIT_SUCCESS;

	if (priv->qlen > priv->qlen_limit) {
		priv->packets_dropped++;
		sch->qstats.drops++;
		qdisc_drop(skb, sch, to_free);
		return NET_XMIT_DROP;
	}


	list_add_tail(&skb->list, &priv->q);
	priv->qlen++;
	sch->qstats.qlen++;

	return ret;
}

// TODO: How to deactivate a q ????
// - watchdog counter on each qdisc

// old*0.875+new*0.125
#define MOV_AVG(old, tdiff, len) ((old>>1)+(old>>2)+(old>>3) + (div64_u64(len*NSEC_PER_SEC,tdiff)>>3))
static struct sk_buff *mc_qdisc_dequeue(struct Qdisc *sch)
{
	struct sk_buff *s;
	int num_active_qs = 1;
	u64 now;
	struct mc_sched_data *priv = qdisc_priv(sch);
	u64 len;
	struct list_head *pos;

	if (list_empty(&priv->q))	{
		sch->qstats.backlog++;
		return NULL;
	}

	if (priv->packets_sent)
		WRITE_ONCE(priv->active, true);

	/*send immediately*/
	if (priv->max_rate == 0) {
		s = list_first_entry(&priv->q, struct sk_buff, list);
		list_del(&s->list);
		priv->packets_sent++;
		sch->qstats.qlen--;
		priv->qlen--;
		return s;
	}
	

	now = ktime_get_ns();

	if (now-priv->last_checked_active >= priv->sync_time) { //check every 100ms is the default
		rcu_read_lock();
		list_for_each_rcu(pos, &priv->mc_list) {
			struct mc_sched_data *other_priv = container_of(pos, struct mc_sched_data, mc_list);
			u64 other_pkts_sent = READ_ONCE(other_priv->packets_sent);
			if (other_pkts_sent != priv->qdisc_wd_active[other_priv->txq_num]) {
				num_active_qs++;
			}
			priv->qdisc_wd_active[other_priv->txq_num] = other_pkts_sent;
		}
		rcu_read_unlock();
		priv->last_checked_active = now;
		priv->last_active_qs = num_active_qs;
		sch->qstats.requeues++;
		priv->total_nr_of_active_qs += num_active_qs;
		priv->nr_of_sync_loops++;
	}

	s = list_first_entry(&priv->q, struct sk_buff, list);

	if (priv->time_next_packet <= now) {
		priv->qlen--;
		sch->qstats.qlen--;
		list_del(&s->list);
		priv->packets_sent++;
		
		len = qdisc_pkt_len(s)*NSEC_PER_SEC*(priv->last_active_qs);
		len = div64_ul(len, priv->max_rate);

		if (priv->time_next_packet)
			len -= min(len/2, now-priv->time_next_packet);

		priv->time_next_packet = now+len;
	}
	else {
		if (priv->time_next_packet != priv->last_watchdog ){
			sch->qstats.overlimits++;
			qdisc_watchdog_schedule_range_ns(&priv->watchdog, priv->time_next_packet, priv->wd_slack);
			priv->last_watchdog = priv->time_next_packet;
		}
		s = NULL;
	}

	return s;
}

static int mc_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct mc_sched_data *q = qdisc_priv(sch);
	struct nlattr *opts;
	u64 active_queues = 0;

	opts = nla_nest_start_noflag(skb, TCA_OPTIONS);
	if (!opts)
		goto nla_put_failure;

	if (nla_put_u32(skb, TCA_MC_MAX_RATE, q->max_rate))
		goto nla_put_failure;

	if (nla_put_u64_64bit(skb, TCA_MC_PACKETS_SENT, q->packets_sent,1))
		goto nla_put_failure;

	if (q->nr_of_sync_loops != 0) {
		active_queues = div_u64(q->total_nr_of_active_qs, q->nr_of_sync_loops);
	}

	if (nla_put_u64_64bit(skb, TCA_MC_ACTIVE_Q_AVG, active_queues,1))
		goto nla_put_failure;

	return nla_nest_end(skb, opts);

nla_put_failure:
	return -1;
}

static const struct nla_policy mc_policy[TCA_MC_MAX + 1] = {
	[TCA_MC_MAX_RATE]		= { .type = NLA_U32 },
	[TCA_MC_PACKETS_SENT]		= { .type = NLA_U64 },
	[TCA_MC_SYNC_TIME]		= { .type = NLA_U32 },
	[TCA_MC_QLEN_LIMIT]		= { .type = NLA_U32 },
	[TCA_MC_WD_SLACK]		= { .type = NLA_U32 },
	[TCA_MC_ACTIVE_Q_AVG]		= { .type = NLA_U64 },
};

static int mc_change(struct Qdisc *sch, struct nlattr *opt,
		struct netlink_ext_ack *extack)
{
	struct mc_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_MC_MAX+1];
	int err = 0;

	err = nla_parse_nested_deprecated(tb, TCA_MC_MAX, opt, mc_policy, NULL);
	if (err < 0) {
		return err;
	}

	sch_tree_lock(sch);

	if (tb[TCA_MC_MAX_RATE]) {
		u32 rate = nla_get_u32(tb[TCA_MC_MAX_RATE]);

		q->max_rate = (rate == ~0U) ? ~0 : rate;
	}
	if (tb[TCA_MC_SYNC_TIME]) {
		u32 sync_us = nla_get_u32(tb[TCA_MC_SYNC_TIME]);
		q->sync_time = (u64)sync_us * 1000; // In us
	}
	if (tb[TCA_MC_QLEN_LIMIT]) {
		u32 qlen_limit = nla_get_u32(tb[TCA_MC_QLEN_LIMIT]);
		q->qlen_limit = qlen_limit; // In us
	}
	if (tb[TCA_MC_WD_SLACK]) {
		u32 wd_slack = nla_get_u32(tb[TCA_MC_WD_SLACK]);
		q->wd_slack = wd_slack; // In us
	}

	sch_tree_unlock(sch);
	return err;
}

static void mc_destroy(struct Qdisc *sch) 
{
	struct mc_sched_data *priv = qdisc_priv(sch);
	spinlock_t *root_lock;

	pr_err("qdisc %d: packets sent %llu", priv->txq_num, priv->packets_sent);

	root_lock = qdisc_lock(qdisc_root(sch));
	spin_lock(root_lock);
	//is this safe? Yes, __qdisc_destroy calls the free function with call_rcu
	list_del_rcu(&priv->mc_list);
	spin_unlock(root_lock);

	qdisc_watchdog_cancel(&priv->watchdog);
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
	u32 num_of_qdiscs = 0;
    pr_err("In: %s v0.3-dev %lx\n", __func__, (unsigned long)sch);
	INIT_LIST_HEAD(&(priv->q));

	priv->last_enqueued = 0;
	priv->max_rate = 0;
	priv->active = 0;
	priv->last_checked_active = 0;
	priv->packets_sent = 0;
	priv->sync_time = SYNC_PERIOD;
	priv->qlen_limit = QLEN_LIMIT_DEFAULT;
	priv->wd_slack = WD_SLACK;
	priv->num_tx_queues = 0;
	priv->last_watchdog = 0;
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
		num_of_qdiscs++;
		pr_err("list: %u\n", priv->txq_num);
	}
	priv->txq_num = num_of_qdiscs;
	lpriv = container_of(pos, struct mc_sched_data, mc_list);
	pr_err("list: %u\n", lpriv->txq_num);

	if (opt)
		err = mc_change(sch, opt, extack);

	pr_err("rate: %u\n", priv->max_rate);
	pr_err("sync_time: %llu\n", priv->sync_time);
	pr_err("qlen_limit: %u\n", priv->qlen_limit);
	pr_err("slack: %u\n", priv->wd_slack);

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
