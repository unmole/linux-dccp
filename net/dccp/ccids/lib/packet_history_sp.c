/*
 *  Copyright (c) 2007   The University of Aberdeen, Scotland, UK
 *  Copyright (c) 2005-7 The University of Waikato, Hamilton, New Zealand.
 *
 *  An implementation of the DCCP protocol
 *  Copyright (c) 2009 Ivo Calado, Erivaldo Xavier, Leandro Sales
 *
 *  This code has been developed by the Federal University of Campina Grande
 *  Embedded Systems and Pervasive Computing Lab. For further information
 *  please see http://embedded.ufcg.edu.br/
 *  <ivocalado@embedded.ufcg.edu.br> <desadoc@gmail.com> <leandroal@gmail.com>
 *
 *  This code has been developed by the University of Waikato WAND
 *  research group. For further information please see http://www.wand.net.nz/
 *  or e-mail Ian McDonald - ian.mcdonald@jandi.co.nz
 *
 *  This code also uses code from Lulea University, rereleased as GPL by its
 *  authors:
 *  Copyright (c) 2003 Nils-Erik Mattsson, Joacim Haggmark, Magnus Erixzon
 *
 *  Changes to meet Linux coding standards, to make it meet latest ccid3 draft
 *  and to make it work as a loadable module in the DCCP stack written by
 *  Arnaldo Carvalho de Melo <acme@conectiva.com.br>.
 *
 *  Copyright (c) 2005 Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/string.h>
#include <linux/slab.h>
#include "packet_history_sp.h"
#include "../../dccp.h"

/*
 * Transmitter History Routines
 */
static struct kmem_cache *tfrc_tx_hist_slab;

int __init tfrc_sp_tx_packet_history_init(void)
{
	tfrc_tx_hist_slab = kmem_cache_create("tfrc_sp_tx_hist",
					      sizeof(struct tfrc_tx_hist_entry),
					      0, SLAB_HWCACHE_ALIGN, NULL);
	return tfrc_tx_hist_slab == NULL ? -ENOBUFS : 0;
}

void tfrc_sp_tx_packet_history_exit(void)
{
	if (tfrc_tx_hist_slab != NULL) {
		kmem_cache_destroy(tfrc_tx_hist_slab);
		tfrc_tx_hist_slab = NULL;
	}
}

int tfrc_sp_tx_hist_add(struct tfrc_tx_hist_entry **headp, u64 seqno, u8 ccval)
{
	struct tfrc_tx_hist_entry *entry =
		kmem_cache_alloc(tfrc_tx_hist_slab, gfp_any());

	if (entry == NULL)
		return -ENOBUFS;
	entry->seqno = seqno;
	entry->ccval = ccval;
	entry->stamp = ktime_get_real();
	entry->next  = *headp;
	*headp	     = entry;
	return 0;
}

void tfrc_sp_tx_hist_purge(struct tfrc_tx_hist_entry **headp)
{
	struct tfrc_tx_hist_entry *head = *headp;

	while (head != NULL) {
		struct tfrc_tx_hist_entry *next = head->next;

		kmem_cache_free(tfrc_tx_hist_slab, head);
		head = next;
	}

	*headp = NULL;
}

/*
 *	Receiver History Routines
 */
static struct kmem_cache *tfrc_rx_hist_slab;

int __init tfrc_sp_rx_packet_history_init(void)
{
	tfrc_rx_hist_slab = kmem_cache_create("tfrc_sp_rxh_cache",
					      sizeof(struct tfrc_rx_hist_entry),
					      0, SLAB_HWCACHE_ALIGN, NULL);
	return tfrc_rx_hist_slab == NULL ? -ENOBUFS : 0;
}

void tfrc_sp_rx_packet_history_exit(void)
{
	if (tfrc_rx_hist_slab != NULL) {
		kmem_cache_destroy(tfrc_rx_hist_slab);
		tfrc_rx_hist_slab = NULL;
	}
}

static inline void
	tfrc_sp_rx_hist_entry_from_skb(struct tfrc_rx_hist_entry *entry,
					       const struct sk_buff *skb,
					       const u64 ndp)
{
	const struct dccp_hdr *dh = dccp_hdr(skb);

	entry->tfrchrx_seqno = DCCP_SKB_CB(skb)->dccpd_seq;
	entry->tfrchrx_ccval = dh->dccph_ccval;
	entry->tfrchrx_type  = dh->dccph_type;
	entry->tfrchrx_ndp   = ndp;
	entry->tfrchrx_tstamp = ktime_get_real();
}

void tfrc_sp_rx_hist_add_packet(struct tfrc_rx_hist *h,
			     const struct sk_buff *skb,
			     const u64 ndp)
{
	struct tfrc_rx_hist_entry *entry = tfrc_rx_hist_last_rcv(h);

	tfrc_sp_rx_hist_entry_from_skb(entry, skb, ndp);
}

/* has the packet contained in skb been seen before? */
int tfrc_sp_rx_hist_duplicate(struct tfrc_rx_hist *h, struct sk_buff *skb)
{
	const u64 seq = DCCP_SKB_CB(skb)->dccpd_seq,
		  prev_seq = tfrc_rx_hist_loss_prev(h)->tfrchrx_seqno;
	int i;

	if (dccp_delta_seqno(prev_seq, seq) <= 0)
		return 1;

	for (i = 1; i <= h->loss_count; i++)
		if (tfrc_rx_hist_entry(h, i)->tfrchrx_seqno == seq)
			return 1;

	return 0;
}


static void __tfrc_rx_hist_swap(struct tfrc_rx_hist *h, const u8 a, const u8 b)
{
	struct tfrc_rx_hist_entry *tmp = h->ring[a];

	h->ring[a] = h->ring[b];
	h->ring[b] = tmp;
}

static void tfrc_rx_hist_swap(struct tfrc_rx_hist *h, const u8 a, const u8 b)
{
	__tfrc_rx_hist_swap(h, tfrc_rx_hist_index(h, a),
			       tfrc_rx_hist_index(h, b));
}

/**
 * tfrc_rx_hist_resume_rtt_sampling  -  Prepare RX history for RTT sampling
 * This is called after loss detection has finished, when the history entry
 * with the index of `loss_count' holds the highest-received sequence number.
 * RTT sampling requires this information at ring[0] (tfrc_rx_hist_sample_rtt).
 */
static inline void tfrc_rx_hist_resume_rtt_sampling(struct tfrc_rx_hist *h)
{
	__tfrc_rx_hist_swap(h, 0, tfrc_rx_hist_index(h, h->loss_count));
	h->loss_count = h->loss_start = 0;
}

/*
 * Private helper functions for loss detection.
 *
 * In the descriptions, `Si' refers to the sequence number of entry number i,
 * whose NDP count is `Ni' (lower case is used for variables).
 * Note: All __xxx_loss functions expect that a test against duplicates has been
 *       performed already: the seqno of the skb must not be less than the seqno
 *       of loss_prev; and it must not equal that of any valid history entry.
 */
static void __do_track_loss(struct tfrc_rx_hist *h, struct sk_buff *skb, u64 n1)
{
	u64 s0 = tfrc_rx_hist_loss_prev(h)->tfrchrx_seqno,
	    s1 = DCCP_SKB_CB(skb)->dccpd_seq;

	if (!dccp_loss_free(s0, s1, n1))	/* gap between S0 and S1 */
		h->loss_count = 1;
}

static void __one_after_loss(struct tfrc_rx_hist *h,
			     struct sk_buff *skb, u32 n2)
{
	u64 s0 = tfrc_rx_hist_loss_prev(h)->tfrchrx_seqno,
	    s1 = tfrc_rx_hist_entry(h, 1)->tfrchrx_seqno,
	    s2 = DCCP_SKB_CB(skb)->dccpd_seq;

	if (likely(dccp_delta_seqno(s1, s2) > 0)) {	/* S1  <  S2 */
		h->loss_count = 2;
		tfrc_sp_rx_hist_entry_from_skb(tfrc_rx_hist_entry(h, 2),
					       skb, n2);
		return;
	}

	/* S0  <  S2  <  S1 */

	if (dccp_loss_free(s0, s2, n2)) {
		u64 n1 = tfrc_rx_hist_entry(h, 1)->tfrchrx_ndp;

		if (dccp_loss_free(s2, s1, n1)) {
			/* hole is filled: S0, S2, and S1 are consecutive */
			tfrc_rx_hist_resume_rtt_sampling(h);
		} else
			/* gap between S2 and S1: just update loss_prev */
			tfrc_sp_rx_hist_entry_from_skb(
					tfrc_rx_hist_loss_prev(h), skb, n2);

	} else {	/* gap between S0 and S2 */
		/*
		 * Reorder history to insert S2 between S0 and S1
		 */
		tfrc_rx_hist_swap(h, 0, 3);
		h->loss_start = tfrc_rx_hist_index(h, 3);
		tfrc_sp_rx_hist_entry_from_skb(tfrc_rx_hist_entry(h, 1),
					       skb, n2);
		h->loss_count = 2;
	}
}

/* return 1 if a new loss event has been identified */
static int __two_after_loss(struct tfrc_rx_hist *h, struct sk_buff *skb, u32 n3)
{
	u64 s0 = tfrc_rx_hist_loss_prev(h)->tfrchrx_seqno,
	    s1 = tfrc_rx_hist_entry(h, 1)->tfrchrx_seqno,
	    n1 = tfrc_rx_hist_entry(h, 1)->tfrchrx_ndp,
	    s2 = tfrc_rx_hist_entry(h, 2)->tfrchrx_seqno,
	    s3 = DCCP_SKB_CB(skb)->dccpd_seq;

	if (likely(dccp_delta_seqno(s2, s3) > 0)) {	/* S2  <  S3 */
		h->loss_count = 3;
		tfrc_sp_rx_hist_entry_from_skb(tfrc_rx_hist_entry(h, 3),
					       skb, n3);
		h->num_losses = dccp_loss_count(s0, s1, n1);
		return 1;
	}

	/* S3  <  S2 */

	if (dccp_delta_seqno(s1, s3) > 0) {		/* S1  <  S3  <  S2 */
		/*
		 * Reorder history to insert S3 between S1 and S2
		 */
		tfrc_rx_hist_swap(h, 2, 3);
		tfrc_sp_rx_hist_entry_from_skb(tfrc_rx_hist_entry(h, 2),
					       skb, n3);
		h->loss_count = 3;
		h->num_losses = dccp_loss_count(s0, s1, n1);
		return 1;
	}

	/* S0  <  S3  <  S1 */

	if (dccp_loss_free(s0, s3, n3)) {

		if (dccp_loss_free(s3, s1, n1)) {
			/* hole between S0 and S1 filled by S3 */
			u64 n2 = tfrc_rx_hist_entry(h, 2)->tfrchrx_ndp;

			if (dccp_loss_free(s1, s2, n2)) {
				/* entire hole filled by S0, S3, S1, S2 */
				tfrc_rx_hist_resume_rtt_sampling(h);
			} else {
				/* gap remains between S1 and S2 */
				h->loss_start = tfrc_rx_hist_index(h, 1);
				h->loss_count = 1;
			}

		} else /* gap exists between S3 and S1, loss_count stays at 2 */
			tfrc_sp_rx_hist_entry_from_skb(
					tfrc_rx_hist_loss_prev(h), skb, n3);

		return 0;
	}

	/*
	 * The remaining case:  S0  <  S3  <  S1  <  S2;  gap between S0 and S3
	 * Reorder history to insert S3 between S0 and S1.
	 */
	tfrc_rx_hist_swap(h, 0, 3);
	h->loss_start = tfrc_rx_hist_index(h, 3);
	tfrc_sp_rx_hist_entry_from_skb(tfrc_rx_hist_entry(h, 1), skb, n3);
	h->loss_count = 3;
	h->num_losses = dccp_loss_count(s0, s3, n3);

	return 1;
}

/* recycle RX history records to continue loss detection if necessary */
static void __three_after_loss(struct tfrc_rx_hist *h)
{
	/*
	 * At this stage we know already that there is a gap between S0 and S1
	 * (since S0 was the highest sequence number received before detecting
	 * the loss). To recycle the loss record, it is	thus only necessary to
	 * check for other possible gaps between S1/S2 and between S2/S3.
	 */
	u64 s1 = tfrc_rx_hist_entry(h, 1)->tfrchrx_seqno,
	    s2 = tfrc_rx_hist_entry(h, 2)->tfrchrx_seqno,
	    s3 = tfrc_rx_hist_entry(h, 3)->tfrchrx_seqno;
	u64 n2 = tfrc_rx_hist_entry(h, 2)->tfrchrx_ndp,
	    n3 = tfrc_rx_hist_entry(h, 3)->tfrchrx_ndp;

	if (dccp_loss_free(s1, s2, n2)) {

		if (dccp_loss_free(s2, s3, n3)) {
			/* no gap between S2 and S3: entire hole is filled */
			tfrc_rx_hist_resume_rtt_sampling(h);
		} else {
			/* gap between S2 and S3 */
			h->loss_start = tfrc_rx_hist_index(h, 2);
			h->loss_count = 1;
		}

	} else {	/* gap between S1 and S2 */
		h->loss_start = tfrc_rx_hist_index(h, 1);
		h->loss_count = 2;
	}
}

/**
 *  tfrc_rx_congestion_event  -  Loss detection and further processing
 *  @h:		The non-empty RX history object
 *  @lh:	Loss Intervals database to update
 *  @skb:	Currently received packet
 *  @ndp:	The NDP count belonging to @skb
 *  @first_li:	Caller-dependent computation of first loss interval in @lh
 *  @sk:	Used by @calc_first_li (see tfrc_lh_interval_add)
 *  Chooses action according to pending loss, updates LI database when a new
 *  loss was detected, and does required post-processing. Returns 1 when caller
 *  should send feedback, 0 otherwise.
 *  Since it also takes care of reordering during loss detection and updates the
 *  records accordingly, the caller should not perform any more RX history
 *  operations when loss_count is greater than 0 after calling this function.
 */
bool tfrc_sp_rx_congestion_event(struct tfrc_rx_hist *h,
				 struct tfrc_loss_hist *lh,
				 struct tfrc_loss_data *ld,
				 struct sk_buff *skb, const u64 ndp,
				 u32 (*first_li)(struct sock *),
				 struct sock *sk)
{
	bool new_loss = false, new_event = false;

	if (tfrc_sp_rx_hist_duplicate(h, skb))
		return 0;

	if (h->loss_count == 0) {
		__do_track_loss(h, skb, ndp);
		tfrc_sp_rx_hist_sample_rtt(h, skb);
		tfrc_sp_rx_hist_add_packet(h, skb, ndp);
	} else if (h->loss_count == 1) {
		__one_after_loss(h, skb, ndp);
	} else if (h->loss_count != 2) {
		DCCP_BUG("invalid loss_count %d", h->loss_count);
	} else if (__two_after_loss(h, skb, ndp)) {
		/*
		 * Update Loss Interval database and recycle RX records
		 */
		new_loss = true;
		new_event = tfrc_sp_lh_interval_add(lh, h, first_li, sk);
		__three_after_loss(h);

	} else if (dccp_data_packet(skb) && dccp_skb_is_ecn_ce(skb)) {
		/*
		 * We only test for ECN marks on data packets. This is the way
		 * the RFC considers ECN marks - a future implementation may
		 * find it useful to also check ECN marks on non-data packets.
		 */
		new_event = tfrc_sp_lh_interval_add(lh, h, first_li, sk);
		/*
		 * Also combinations of loss and ECN-marks (as per the warning)
		 * are not supported. The permutations of loss combined with or
		 * without ECN-marks create a lot of complexity, which does not
		 * seem warranted: ECN-marked packets immediately trigger con-
		 * gestion events.
		 * Testing is needed to see whether ignoring the loss_count in
		 * light of an ECN-marked-CE event really has any disadvantages.
		 */
		if (new_event && h->loss_count) {
			DCCP_WARN("Ignoring loss count due to ECN\n");
			tfrc_rx_hist_resume_rtt_sampling(h);
		}
	}

	/*
	 * Update Loss Interval data used for options
	 */
	tfrc_sp_update_li_data(ld, h, skb, new_loss, new_event);

	/*
	 * Update moving-average of `s' and the sum of received payload bytes.
	 */
	if (dccp_data_packet(skb)) {
		const u32 payload = skb->len - dccp_hdr(skb)->dccph_doff * 4;

		h->packet_size = tfrc_ewma(h->packet_size, payload, 9);
		h->bytes_recvd += payload;
	}

	/* RFC 3448, 6.1: update I_0, whose growth implies p <= p_prev */
	if (!new_event)
		tfrc_sp_lh_update_i_mean(lh, skb);

	return new_event;
}

/* Compute the sending rate X_recv measured between feedback intervals */
u32 tfrc_sp_rx_hist_x_recv(struct tfrc_rx_hist *h, const u32 last_x_recv)
{
	u64 bytes = h->bytes_recvd, last_rtt = h->rtt_estimate;
	s64 delta = ktime_to_us(net_timedelta(h->bytes_start));

	WARN_ON(delta <= 0);
	/*
	 * Ensure that the sampling interval for X_recv is at least one RTT,
	 * by extending the sampling interval backwards in time, over the last
	 * R_(m-1) seconds, as per rfc3448bis-06, 6.2.
	 * To reduce noise (e.g. when the RTT changes often), this is only
	 * done when delta is smaller than RTT/2.
	 */
	if (last_x_recv > 0 && delta < last_rtt/2) {
		tfrc_sp_pr_debug("delta < RTT ==> %ld us < %u us\n",
			      (long)delta, (unsigned int)last_rtt);

		delta = (bytes ? delta : 0) + last_rtt;
		bytes += div_u64((u64)last_x_recv * last_rtt, USEC_PER_SEC);
	}

	if (unlikely(bytes == 0)) {
		DCCP_WARN("X_recv == 0, using old value of %u\n", last_x_recv);
		return last_x_recv;
	}
	return scaled_div32(bytes, delta);
}

void tfrc_sp_rx_hist_purge(struct tfrc_rx_hist *h)
{
	int i;

	for (i = 0; i <= TFRC_NDUPACK; ++i)
		if (h->ring[i] != NULL) {
			kmem_cache_free(tfrc_rx_hist_slab, h->ring[i]);
			h->ring[i] = NULL;
		}
}

static int tfrc_sp_rx_hist_alloc(struct tfrc_rx_hist *h)
{
	int i;

	memset(h, 0, sizeof(*h));

	for (i = 0; i <= TFRC_NDUPACK; i++) {
		h->ring[i] = kmem_cache_alloc(tfrc_rx_hist_slab, GFP_ATOMIC);
		if (h->ring[i] == NULL) {
			tfrc_sp_rx_hist_purge(h);
			return -ENOBUFS;
		}
	}
	return 0;
}

int tfrc_sp_rx_hist_init(struct tfrc_rx_hist *h, struct sock *sk)
{
	if (tfrc_sp_rx_hist_alloc(h))
		return -ENOBUFS;
	/*
	 * Initialise first entry with GSR to start loss detection as early as
	 * possible. Code using this must not use any other fields. The entry
	 * will be overwritten once the CCID updates its received packets.
	 */
	tfrc_rx_hist_loss_prev(h)->tfrchrx_seqno = dccp_sk(sk)->dccps_gsr;
	return 0;
}

/**
 * tfrc_rx_hist_sample_rtt  -  Sample RTT from timestamp / CCVal
 * Based on ideas presented in RFC 4342, 8.1. This function expects that no loss
 * is pending and uses the following history entries (via rtt_sample_prev):
 * - h->ring[0]  contains the most recent history entry prior to @skb;
 * - h->ring[1]  is an unused `dummy' entry when the current difference is 0;
 */
void tfrc_sp_rx_hist_sample_rtt(struct tfrc_rx_hist *h,
				const struct sk_buff *skb)
{
	struct tfrc_rx_hist_entry *last = h->ring[0];
	u32 sample, delta_v;
	s64 t_us;

	/*
	 * When not to sample:
	 * - on non-data packets
	 *   (RFC 4342, 8.1: CCVal only fully defined for data packets);
	 * - when no data packets have been received yet
	 *   (FIXME: using sampled packet size as indicator here);
	 * - as long as there are gaps in the sequence space (pending loss).
	 */
	if (!dccp_data_packet(skb) || h->packet_size == 0 ||
	    tfrc_rx_hist_loss_pending(h))
		return;

	h->rtt_sample_prev = 0;		/* reset previous candidate */

	delta_v = SUB16(dccp_hdr(skb)->dccph_ccval, last->tfrchrx_ccval);
	if (delta_v == 0) {		/* less than RTT/4 difference */
		h->rtt_sample_prev = 1;
		return;
	}
	t_us = ktime_to_us(net_timedelta(last->tfrchrx_tstamp));
	sample = dccp_sane_rtt(t_us);

	if (delta_v <= 4)		/* between RTT/4 and RTT */
		sample *= 4 / delta_v;
	else if (!(sample < h->rtt_estimate && sample > h->rtt_estimate/2))
		/*
		 * Optimisation: CCVal difference is greater than 1 RTT, yet the
		 * sample is less than the local RTT estimate; which means that
		 * the RTT estimate is too high.
		 * To avoid noise, it is not done if the sample is below RTT/2.
		 */
		return;

	/* Use a lower weight than usual to increase responsiveness */
	h->rtt_estimate = tfrc_ewma(h->rtt_estimate, sample, 5);
}
