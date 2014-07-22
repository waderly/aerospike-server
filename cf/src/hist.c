/*
 * hist.c
 *
 * Copyright (C) 2008-2014 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

#include "hist.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "citrusleaf/alloc.h"
#include "citrusleaf/cf_atomic.h"
#include "citrusleaf/cf_bits.h"
#include "citrusleaf/cf_clock.h"

#include "dynbuf.h"
#include "fault.h"


//==========================================================
// Histogram with logarithmic buckets.
//

//------------------------------------------------
// Create a histogram. There's no destroy(), but
// you can just cf_free() the histogram.
//
histogram*
histogram_create(const char *name)
{
	if (! (name && strlen(name) < HISTOGRAM_NAME_SIZE)) {
		return NULL;
	}

	histogram *h = cf_malloc(sizeof(histogram));

	if (! h) {
		return NULL;
	}

	strcpy(h->name, name);
	h->total_count = 0;
	memset(&h->counts, 0, sizeof(h->counts));

	return h;
}

//------------------------------------------------
// Clear a histogram.
//
void
histogram_clear(histogram *h)
{
	cf_atomic_int_set(&h->total_count, 0);

	for (int i = 0; i < N_BUCKETS; i++) {
		cf_atomic_int_set(&h->counts[i], 0);
	}
}

//------------------------------------------------
// Dump a histogram to log.
//
// Note - DO NOT change the log output format in
// this method - tools such as as_log_latency
// assume this format.
//
void
histogram_dump(histogram *h)
{
	cf_info(AS_INFO, "histogram dump: %s (%zu total)", h->name, h->total_count);

	int i, j;
	int k = 0;

	for (j = N_BUCKETS - 1; j >= 0; j--) {
		if (h->counts[j]) {
			break;
		}
	}

	for (i = 0; i < N_BUCKETS; i++) {
		if (h->counts[i]) {
			break;
		}
	}

	char buf[100];
	int pos = 0;

	buf[0] = '\0';

	for ( ; i <= j; i++) {
		if (h->counts[i] > 0) { // print only non-zero columns
			int bytes = sprintf(buf + pos, " (%02d: %010zu) ", i, h->counts[i]);

			if (bytes <= 0) {
				cf_info(AS_INFO, "histogram dump error");
				return;
			}

			pos += bytes;

			if (k % 4 == 3) {
				 cf_info(AS_INFO, "%s", buf);
				 pos = 0;
				 buf[0] = '\0';
			}

			k++;
		}
	}

	if (pos > 0) {
		cf_info(AS_INFO, "%s", buf);
	}
}

//------------------------------------------------
// BYTE_MSB[n] returns the position of the most
// significant bit. If no bits are set (n = 0) it
// returns 0. Otherwise the positions are 1 ... 8
// from low to high, so e.g. n = 13 returns 4:
//
//		bits:		0  0  0  0  1  1  0  1
//		position:	8  7  6  5 [4] 3  2  1
//
static const char BYTE_MSB[] = {
		0, 1, 2, 2, 3, 3, 3, 3,  4, 4, 4, 4, 4, 4, 4, 4,
		5, 5, 5, 5, 5, 5, 5, 5,  5, 5, 5, 5, 5, 5, 5, 5,
		6, 6, 6, 6, 6, 6, 6, 6,  6, 6, 6, 6, 6, 6, 6, 6,
		6, 6, 6, 6, 6, 6, 6, 6,  6, 6, 6, 6, 6, 6, 6, 6,

		7, 7, 7, 7, 7, 7, 7, 7,  7, 7, 7, 7, 7, 7, 7, 7,
		7, 7, 7, 7, 7, 7, 7, 7,  7, 7, 7, 7, 7, 7, 7, 7,
		7, 7, 7, 7, 7, 7, 7, 7,  7, 7, 7, 7, 7, 7, 7, 7,
		7, 7, 7, 7, 7, 7, 7, 7,  7, 7, 7, 7, 7, 7, 7, 7,

		8, 8, 8, 8, 8, 8, 8, 8,  8, 8, 8, 8, 8, 8, 8, 8,
		8, 8, 8, 8, 8, 8, 8, 8,  8, 8, 8, 8, 8, 8, 8, 8,
		8, 8, 8, 8, 8, 8, 8, 8,  8, 8, 8, 8, 8, 8, 8, 8,
		8, 8, 8, 8, 8, 8, 8, 8,  8, 8, 8, 8, 8, 8, 8, 8,

		8, 8, 8, 8, 8, 8, 8, 8,  8, 8, 8, 8, 8, 8, 8, 8,
		8, 8, 8, 8, 8, 8, 8, 8,  8, 8, 8, 8, 8, 8, 8, 8,
		8, 8, 8, 8, 8, 8, 8, 8,  8, 8, 8, 8, 8, 8, 8, 8,
		8, 8, 8, 8, 8, 8, 8, 8,  8, 8, 8, 8, 8, 8, 8, 8
};

//------------------------------------------------
// Returns the position of the most significant
// bit of n. Positions are 1 ... 64 from low to
// high, so:
//
//		n			msb(n)
//		--------	------
//		0			0
//		1			1
//		2 ... 3		2
//		4 ... 7		3
//		8 ... 15	4
//		etc.
//
static int
msb(uint64_t n)
{
	int shift = 0;

	while (true) {
		uint64_t n_div_256 = n >> 8;

		if (n_div_256 == 0) {
			return shift + (int)BYTE_MSB[n];
		}

		n = n_div_256;
		shift += 8;
	}

	// Should never get here.
	cf_crash(AS_INFO, "end of msb()");
	return -1;
}

//------------------------------------------------
// Insert a raw data point.
//
void
histogram_insert_raw(histogram *h, uint64_t value)
{
	cf_atomic_int_incr(&h->total_count);
	cf_atomic_int_incr(&h->counts[msb(value)]);
}

//------------------------------------------------
// Insert a data point. The value is time elapsed
// since start_ns, converted to milliseconds.
// Assumes start_ns was obtained via cf_getns()
// some time ago. These data points generate a
// histogram with:
//
//		index	ms range
//		-----	--------
//		0		0 to 1  (more exactly, 0.999999)
//		1		1 to 2  (more exactly, 1.999999)
//		2		2 to 4  (more exactly, 3.999999)
//		3		4 to 8  (more exactly, 7.999999)
//		4		8 to 16 (more exactly, 15.999999)
//		etc.
//
void
histogram_insert_ms_since(histogram *h, uint64_t start_ns)
{
	cf_atomic_int_incr(&h->total_count);

	uint64_t end_ns = cf_getns();
	uint64_t delta_ms = (end_ns - start_ns) / 1000000;

	int index = 0;

	if (delta_ms != 0) {
		index = msb(delta_ms);

		if (start_ns > end_ns) {
			// Either the clock went backwards, or wrapped. (Assume the former,
			// since it takes ~580 years from 0 to wrap.)
			cf_warning(AS_INFO, "clock went backwards: start %lu end %lu",
					start_ns, end_ns);
			index = 0;
		}
	}

	cf_atomic_int_incr(&h->counts[index]);
}

//------------------------------------------------
// Insert a data point. The value is time elapsed
// since start_ns, converted to microseconds.
// Assumes start_ns was obtained via cf_getns()
// some time ago. These data points generate a
// histogram with:
//
//		index	us range
//		-----	--------
//		0		0 to 1  (more exactly, 0.999)
//		1		1 to 2  (more exactly, 1.999)
//		2		2 to 4  (more exactly, 3.999)
//		3		4 to 8  (more exactly, 7.999)
//		4		8 to 16 (more exactly, 15.999)
//		etc.
//
void
histogram_insert_us_since(histogram *h, uint64_t start_ns)
{
	cf_atomic_int_incr(&h->total_count);

	uint64_t end_ns = cf_getns();
	uint64_t delta_us = (end_ns - start_ns) / 1000;

	int index = 0;

	if (delta_us != 0) {
		index = msb(delta_us);

		if (start_ns > end_ns) {
			// Either the clock went backwards, or wrapped. (Assume the former,
			// since it takes ~580 years from 0 to wrap.)
			cf_warning(AS_INFO, "clock went backwards: start %lu end %lu",
					start_ns, end_ns);
			index = 0;
		}
	}

	cf_atomic_int_incr(&h->counts[index]);
}

// Deprecate:
void
histogram_insert_delta(histogram *h, uint64_t delta)
{
	cf_atomic_int_incr(&h->total_count);

	int index = cf_bits_find_last_set_64(delta);

	if (index < 0) {
		index = 0;
	}

	cf_atomic_int_incr(&h->counts[index]);
}

// Deprecate:
void
histogram_insert_data_point(histogram *h, uint64_t start)
{
	cf_atomic_int_incr(&h->total_count);

	uint64_t end = cf_getms();
	uint64_t delta = end - start;

	int index = cf_bits_find_last_set_64(delta);

	if (index < 0) {
		index = 0;
	}

	if (start > end) {
		// Legend has it this can happen...
		index = 0;
	}

	cf_atomic_int_incr(&h->counts[index]);
}


//==========================================================
// Histogram with linear buckets.
//

//------------------------------------------------
// Create a linear histogram.
//
linear_histogram*
linear_histogram_create(char *name, uint64_t start, uint64_t max_offset,
		int num_buckets)
{
	if (! (name && strlen(name) < HISTOGRAM_NAME_SIZE)) {
		return NULL;
	}

	if (num_buckets > MAX_LINEAR_BUCKETS) {
		cf_crash(AS_INFO, "linear histogram num_buckets %u > max %u",
				num_buckets, MAX_LINEAR_BUCKETS);
	}

	linear_histogram *h = cf_malloc(sizeof(linear_histogram));

	if (! h) {
		return NULL;
	}

	if (0 != pthread_mutex_init(&h->info_lock, NULL)) {
		cf_free(h);
		return NULL;
	}

	strcpy(h->name, name);
	h->num_buckets = num_buckets;
	h->start = start;
	h->bucket_width = max_offset / h->num_buckets;

	// Avoid divide by zero while inserting data point.
	if (h->bucket_width == 0) {
		h->bucket_width = 1;
	}

	h->total_count = 0;
	memset(&h->counts, 0, sizeof(h->counts));
	h->info_snapshot[0] = 0;

	return h;
}

//------------------------------------------------
// Destroy a linear histogram.
//
void
linear_histogram_destroy(linear_histogram *h)
{
	pthread_mutex_destroy(&h->info_lock);
	cf_free(h);
}

//------------------------------------------------
// Clear and re-scale a linear histogram.
//
// Note: not thread safe!
//
void
linear_histogram_clear(linear_histogram *h, uint64_t start, uint64_t max_offset)
{
	h->start = start;
	h->bucket_width = max_offset / h->num_buckets;

	// Avoid divide by zero while inserting data point.
	if (h->bucket_width == 0) {
		h->bucket_width = 1;
	}

	h->total_count = 0;
	memset(&h->counts, 0, sizeof(h->counts));
}

//------------------------------------------------
// Dump a linear histogram to log.
//
// Note - DO NOT change the log output format in
// this method - public documentation assumes this
// format.
//
void
linear_histogram_dump(linear_histogram *h)
{
	cf_debug(AS_NSUP, "linear histogram dump: %s [%u %u]/[%u] (%zu total)",
			h->name, h->start, h->start + (h->num_buckets * h->bucket_width),
			h->bucket_width, h->total_count);

	int i, j;
	int k = 0;

	for (j = h->num_buckets - 1; j >= 0; j--) {
		if (h->counts[j]) {
			break;
		}
	}

	for (i = 0; i < h->num_buckets; i++) {
		if (h->counts[i]) {
			break;
		}
	}

	char buf[100];
	int pos = 0;

	buf[0] = '\0';

	for ( ; i <= j; i++) {
		if (h->counts[i] > 0) { // print only non-zero columns
			int bytes = sprintf(buf + pos, " (%02d: %010zu) ", i, h->counts[i]);

			if (bytes <= 0) {
				cf_debug(AS_NSUP, "linear histogram dump error");
				return;
			}

			pos += bytes;

			if (k % 4 == 3) {
				 cf_debug(AS_NSUP, "%s", buf);
				 pos = 0;
				 buf[0] = '\0';
			}

			k++;
		}
	}

	if (pos > 0) {
		cf_debug(AS_NSUP, "%s", buf);
	}
}

//------------------------------------------------
// Access method for total count.
//
uint64_t
linear_histogram_get_total(linear_histogram *h)
{
	return cf_atomic_int_get(h->total_count);
}

//------------------------------------------------
// Insert a data point. Points out of range will
// end up in the bucket at the appropriate end.
//
void
linear_histogram_insert_data_point(linear_histogram *h, uint64_t point)
{
	cf_atomic_int_incr(&h->total_count);

	int64_t offset = point - h->start;
	int64_t index = 0;

	if (offset > 0) {
		index = offset / h->bucket_width;

		if (index >= (int64_t)h->num_buckets) {
			index = h->num_buckets - 1;
		}
	}

	cf_atomic_int_incr(&h->counts[index]);
}

//------------------------------------------------
// Find how many buckets (from the low end) it
// takes to accumulate the specified percentage of
// total count.
//
// Note: not thread safe!
//
size_t
linear_histogram_get_index_for_pct(linear_histogram *h, size_t pct)
{
	if (h->total_count == 0) {
		return 1;
	}

	int min_limit = (h->total_count * pct) / 100;

	if (min_limit >= h->total_count) {
		return h->num_buckets;
	}

	int count = 0;

	for (int i = 0; i < h->num_buckets; i++) {
		count += h->counts[i];

		if (count >= min_limit) {
			return i + 1;
		}
	}

	return h->num_buckets;
}

//------------------------------------------------
// Get details of the "threshold" bucket - the
// bucket at which the specified percentage of
// total count has been accumulated, starting from
// low bucket.
//
// Note: not thread safe!
//
bool
linear_histogram_get_thresholds_for_fraction(linear_histogram *h,
		uint32_t tenths_pct, uint64_t *p_low, uint64_t *p_high,
		uint32_t *p_mid_tenths_pct)
{
	return linear_histogram_get_thresholds_for_subtotal(h,
			(h->total_count * tenths_pct) / 1000, p_low, p_high,
			p_mid_tenths_pct);
}

//------------------------------------------------
// Get details of the "threshold" bucket - the
// bucket at which the specified sub-total count
// has been accumulated, starting from low bucket.
//
// Note: not thread safe!
//
bool
linear_histogram_get_thresholds_for_subtotal(linear_histogram *h,
		uint64_t subtotal, uint64_t *p_low, uint64_t *p_high,
		uint32_t *p_mid_tenths_pct)
{
	if (h->total_count == 0) {
		*p_low = 0;
		*p_high = 0;
		*p_mid_tenths_pct = 0;
		return false;
	}

	uint64_t count = 0;
	int i;

	for (i = 0; i < h->num_buckets; i++) {
		count += h->counts[i];

		if (count > subtotal) {
			break;
		}
	}

	if (i == h->num_buckets) {
		// This means subtotal >= h->n_counts.
		*p_low = 0;
		*p_high = 0;
		*p_mid_tenths_pct = 0;
		return true;
	}

	*p_low = h->start + (i * h->bucket_width);
	*p_high = *p_low + h->bucket_width;

	uint64_t bucket_subtotal = h->counts[i] - (count - subtotal);

	// Round up to nearest tenth of a percent.
	*p_mid_tenths_pct =
			((bucket_subtotal * 1000) + h->counts[i] - 1) / h->counts[i];

	return i == h->num_buckets - 1;
}

//------------------------------------------------
// Take a linear histogram "snapshot".
//
void
linear_histogram_save_info(linear_histogram *h)
{
	pthread_mutex_lock(&h->info_lock);

	// Write num_buckets, the bucket width, and the first bucket's count.
	int idx = 0;
	int pos = snprintf(h->info_snapshot, INFO_SNAPSHOT_SIZE, "%d,%ld,%ld",
			h->num_buckets, h->bucket_width,
			cf_atomic_int_get(h->counts[idx++]));

	while (pos < INFO_SNAPSHOT_SIZE && idx < h->num_buckets) {
		pos += snprintf(h->info_snapshot + pos, INFO_SNAPSHOT_SIZE - pos,
				",%ld", h->counts[idx++]);
	}

	pthread_mutex_unlock(&h->info_lock);
}

//------------------------------------------------
// Append a linear histogram "snapshot" to db.
//
void
linear_histogram_get_info(linear_histogram *h, cf_dyn_buf *db)
{
	pthread_mutex_lock(&h->info_lock);
	cf_dyn_buf_append_string(db, h->info_snapshot);
	pthread_mutex_unlock(&h->info_lock);
}
