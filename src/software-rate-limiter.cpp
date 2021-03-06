#include <rte_config.h>
#include <rte_common.h>
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <stdint.h>
#include <rte_ethdev.h> 
#include <rte_mempool.h>
#include <rte_ether.h>
#include <rte_cycles.h>
#include <random>
#include <iostream>
#include <unistd.h>
#include "ring.h"
#include "lifecycle.hpp"

// required for gcc 4.7 for some reason
// ???
#ifndef UINT8_MAX
#define UINT8_MAX 255
#endif
#ifndef UINT16_MAX
#define UINT16_MAX 65535U
#endif

// FIXME: duplicate code (needed for a paper, so the usual quick & dirty hacks)
namespace rate_limiter {
	constexpr int batch_size = 64;
	/* Other option to make the end of this thread (based on time instead of the number of elements in the ring) */
	//constexpr int seconds_without_packets = 5;

	/*
	 * Arbitrary time software rate control main
	 * link_speed: DPDK link speed is expressed in Mbit/s
	 */
	static inline void main_loop(struct rte_ring* ring, uint8_t device, uint16_t queue, uint32_t link_speed) {
		// Wait to slave thread while is filling the ring
		sleep(1);
		uint64_t tsc_hz = rte_get_tsc_hz();
		uint64_t id_cycles = 0;
		struct rte_mbuf* bufs[batch_size];
		double link_bps = link_speed * 1000000.0;
		uint64_t cur = rte_get_tsc_cycles();
		uint64_t next_send = cur;
		while (libmoon::is_running(0) && rte_ring_count(ring) > 0) {
			//#L28:  && ((cur - next_send) < seconds_without_packets * tsc_hz)) {
			int cur_batch_size = batch_size;
			int rc = ring_dequeue(ring, reinterpret_cast<void**>(bufs), cur_batch_size);
			while(rc != 0 && cur_batch_size > 1) {
				cur_batch_size /= 2;
				rc = ring_dequeue(ring, reinterpret_cast<void**>(bufs), cur_batch_size);
			}
		    if (rc == 0) {
		    	for (int i = 0; i < cur_batch_size; i++) {
		        	// desired inter-frame spacing is encoded in the hash 'usr' field (bytes)
		            id_cycles = ((uint64_t) bufs[i]->udata64 * 8 / link_bps) * tsc_hz;
		            next_send += id_cycles;
		            while ((cur = rte_get_tsc_cycles()) < next_send); // waiting
		            while (rte_eth_tx_burst(device, queue, bufs + i, 1) == 0);
		        }
		    }
		    cur = rte_get_tsc_cycles();
		}
		return;
	}
	
	static inline void main_loop_poisson(struct rte_ring* ring, uint8_t device, uint16_t queue, uint32_t target, uint32_t link_speed) {
		uint64_t tsc_hz = rte_get_tsc_hz();
		// control IPGs instead of IDT as IDTs < packet_time are physically impossible
		std::default_random_engine rand;
		uint64_t next_send = 0;
		struct rte_mbuf* bufs[batch_size];
		while (1) {
			int rc = ring_dequeue(ring, reinterpret_cast<void**>(bufs), batch_size);
			uint64_t cur = rte_get_tsc_cycles();
			// nothing sent for 10 ms, restart rate control
			if (((int64_t) cur - (int64_t) next_send) > (int64_t) tsc_hz / 100) {
				next_send = cur;
			}
			if (rc == 0) {
				for (int i = 0; i < batch_size; i++) {
					uint64_t pkt_time = (bufs[i]->pkt_len + 24) * 8 / (link_speed / 1000);
					// ns to cycles
					pkt_time *= (double) tsc_hz / 1000000000.0;
					int64_t avg = (int64_t) (tsc_hz / (1000000000.0 / target) - pkt_time);
					while ((cur = rte_get_tsc_cycles()) < next_send);
					std::exponential_distribution<double> distribution(1.0 / avg);
					double delay = (avg <= 0) ? 0 : distribution(rand);
					next_send += pkt_time + delay;
					while (rte_eth_tx_burst(device, queue, bufs + i, 1) == 0);
				}
			} else if (!libmoon::is_running(0)) {
				return;
			}
		}
	}

	static inline void main_loop_cbr(struct rte_ring* ring, uint8_t device, uint16_t queue, uint32_t target) {
		uint64_t tsc_hz = rte_get_tsc_hz();
		uint64_t id_cycles = (uint64_t) (target / (1000000000.0 / ((double) tsc_hz)));
		uint64_t next_send = 0;
		struct rte_mbuf* bufs[batch_size];
		while (libmoon::is_running(0)) {
			int rc = ring_dequeue(ring, reinterpret_cast<void**>(bufs), batch_size);
			uint64_t cur = rte_get_tsc_cycles();
			// nothing sent for 10 ms, restart rate control
			if (((int64_t) cur - (int64_t) next_send) > (int64_t) tsc_hz / 100) {
				next_send = cur;
			}
			if (rc == 0) {
				for (int i = 0; i < batch_size; i++) {
					while ((cur = rte_get_tsc_cycles()) < next_send);
					next_send += id_cycles;
					while (rte_eth_tx_burst(device, queue, bufs + i, 1) == 0);
				}
			} else if (!libmoon::is_running(0)) {
				return;
			}
		}
	}
}

extern "C" {
	void mg_rate_limiter_cbr_main_loop(rte_ring* ring, uint8_t device, uint16_t queue, uint32_t target) {
		rate_limiter::main_loop_cbr(ring, device, queue, target);
	}

	void mg_rate_limiter_poisson_main_loop(rte_ring* ring, uint8_t device, uint16_t queue, uint32_t target, uint32_t link_speed) {
		rate_limiter::main_loop_poisson(ring, device, queue, target, link_speed);
	}

	void mg_rate_limiter_main_loop(rte_ring* ring, uint8_t device, uint16_t queue, uint32_t link_speed) {
		rate_limiter::main_loop(ring, device, queue, link_speed);
	}
}

