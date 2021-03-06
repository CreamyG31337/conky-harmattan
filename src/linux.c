/* -*- mode: c; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: t -*-
 * vim: ts=4 sw=4 noet ai cindent syntax=c
 *
 * Conky, a system monitor, based on torsmo
 *
 * Any original torsmo code is licensed under the BSD license
 *
 * All code written since the fork of torsmo is licensed under the GPL
 *
 * Please see COPYING for details
 *
 * Copyright (c) 2004, Hannu Saransaari and Lauri Hakkarainen
 * Copyright (c) 2007 Toni Spets
 * Copyright (c) 2005-2010 Brenden Matthews, Philip Kovacs, et. al.
 *	(see AUTHORS)
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "conky.h"
#include "logging.h"
#include "common.h"
#include "linux.h"
#include "net_stat.h"
#include "diskio.h"
#include "bme.c"
#include "temphelper.h"
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#ifndef HAVE_CLOCK_GETTIME
#include <sys/time.h>
#endif
#include <fcntl.h>
#include <unistd.h>
// #include <assert.h>
#include <time.h>
#include "top.h"

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <arpa/inet.h>
#ifdef _NET_IF_H
#define _LINUX_IF_H
#endif
#include <linux/route.h>
#include <math.h>
#include <pthread.h>

/* The following ifdefs were adapted from gkrellm */
#include <linux/major.h>

#if !defined(MD_MAJOR)
#define MD_MAJOR 9
#endif

#if !defined(LVM_BLK_MAJOR)
#define LVM_BLK_MAJOR 58
#endif

#if !defined(NBD_MAJOR)
#define NBD_MAJOR 43
#endif

#ifdef HAVE_IWLIB
#include <iwlib.h>
#endif

#include <dbus/dbus.h>

struct sysfs {
	int fd;
	int arg;
	char devtype[256];
	char type[64];
	float factor, offset;
};

#define SHORTSTAT_TEMPL "%*s %llu %llu %llu"
#define LONGSTAT_TEMPL "%*s %llu %llu %llu "

/* This flag tells the linux routines to use the /proc system where possible,
 * even if other api's are available, e.g. sysinfo() or getloadavg().
 * the reason for this is to allow for /proc-based distributed monitoring.
 * using a flag in this manner creates less confusing code. */
static int prefer_proc = 0;

void prepare_update(void)
{
}

int update_uptime(void)
{
#ifdef HAVE_SYSINFO
	if (!prefer_proc) {
		struct sysinfo s_info;

		sysinfo(&s_info);
		info.uptime = (double) s_info.uptime;
	} else
#endif
	{
		static int rep = 0;
		FILE *fp;

		if (!(fp = open_file("/proc/uptime", &rep))) {
			info.uptime = 0.0;
			return 0;
		}
		fscanf(fp, "%lf", &info.uptime);
		fclose(fp);
	}
	return 0;
}

int check_mount(char *s)
{
	int ret = 0;
	FILE *mtab = fopen("/etc/mtab", "r");

	if (mtab) {
		char buf1[256], buf2[128];

		while (fgets(buf1, 256, mtab)) {
			sscanf(buf1, "%*s %128s", buf2);
			if (!strcmp(s, buf2)) {
				ret = 1;
				break;
			}
		}
		fclose(mtab);
	} else {
		NORM_ERR("Could not open mtab");
	}
	return ret;
}

/* these things are also in sysinfo except Buffers:
 * (that's why I'm reading them from proc) */

int update_meminfo(void)
{
	FILE *meminfo_fp;
	static int rep = 0;

	/* unsigned int a; */
	char buf[256];

	info.mem = info.memmax = info.swap = info.swapfree = info.swapmax = info.bufmem =
		info.buffers = info.cached = info.memfree = info.memeasyfree = 0;

	if (!(meminfo_fp = open_file("/proc/meminfo", &rep))) {
		return 0;
	}

	while (!feof(meminfo_fp)) {
		if (fgets(buf, 255, meminfo_fp) == NULL) {
			break;
		}

		if (strncmp(buf, "MemTotal:", 9) == 0) {
			sscanf(buf, "%*s %llu", &info.memmax);
		} else if (strncmp(buf, "MemFree:", 8) == 0) {
			sscanf(buf, "%*s %llu", &info.memfree);
		} else if (strncmp(buf, "SwapTotal:", 10) == 0) {
			sscanf(buf, "%*s %llu", &info.swapmax);
		} else if (strncmp(buf, "SwapFree:", 9) == 0) {
			sscanf(buf, "%*s %llu", &info.swapfree);
		} else if (strncmp(buf, "Buffers:", 8) == 0) {
			sscanf(buf, "%*s %llu", &info.buffers);
		} else if (strncmp(buf, "Cached:", 7) == 0) {
			sscanf(buf, "%*s %llu", &info.cached);
		}
	}

	info.mem = info.memmax - info.memfree;
	info.memeasyfree = info.memfree;
	info.swap = info.swapmax - info.swapfree;

	info.bufmem = info.cached + info.buffers;

	fclose(meminfo_fp);
	return 0;
}

int get_laptop_mode(void)
{
	FILE *fp;
	int val = -1;

	if ((fp = fopen("/proc/sys/vm/laptop_mode", "r")) != NULL)
		fscanf(fp, "%d\n", &val);
	fclose(fp);
	return val;
}

/* my system says:
 * # cat /sys/block/sda/queue/scheduler
 * noop [anticipatory] cfq
 */
char *get_ioscheduler(char *disk)
{
	FILE *fp;
	char buf[128];

	if (!disk)
		return strndup("n/a", text_buffer_size);

	snprintf(buf, 127, "/sys/block/%s/queue/scheduler", disk);
	if ((fp = fopen(buf, "r")) == NULL) {
		return strndup("n/a", text_buffer_size);
	}
	while (!feof(fp)) {
		fscanf(fp, "%127s", buf);
		if (buf[0] == '[') {
			buf[strlen(buf) - 1] = '\0';
			fclose(fp);
			return strndup(buf + 1, text_buffer_size);
		}
	}
	fclose(fp);
	return strndup("n/a", text_buffer_size);
}

static struct {
	char *iface;
	char *ip;
	int count;
} gw_info;

#define COND_FREE(x) if(x) free(x); x = 0
#define SAVE_SET_STRING(x, y) \
	if (x && strcmp((char *)x, (char *)y)) { \
		free(x); \
		x = strndup("multiple", text_buffer_size); \
	} else if (!x) { \
		x = strndup(y, text_buffer_size); \
	}

void update_gateway_info_failure(const char *reason)
{
	if(reason != NULL) {
		perror(reason);
	}
	//2 pointers to 1 location causes a crash when we try to free them both
	gw_info.iface = strndup("failed", text_buffer_size);
	gw_info.ip = strndup("failed", text_buffer_size);
}


/* Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT */
#define RT_ENTRY_FORMAT "%63s %lx %lx %x %*d %*d %*d %lx %*d %*d %*d\n"

int update_gateway_info(void)
{
	FILE *fp;
	struct in_addr ina;
	char iface[64];
	unsigned long dest, gate, mask;
	unsigned int flags;

	COND_FREE(gw_info.iface);
	COND_FREE(gw_info.ip);
	gw_info.count = 0;

	if ((fp = fopen("/proc/net/route", "r")) == NULL) {
		update_gateway_info_failure("fopen()");
		return 0;
	}

	/* skip over the table header line, which is always present */
	fscanf(fp, "%*[^\n]\n");

	while (!feof(fp)) {
		if(fscanf(fp, RT_ENTRY_FORMAT,
			  iface, &dest, &gate, &flags, &mask) != 5) {
			update_gateway_info_failure("fscanf()");
			break;
		}
		if (!(dest || mask) && ((flags & RTF_GATEWAY) || !gate) ) {
			gw_info.count++;
			SAVE_SET_STRING(gw_info.iface, iface)
			ina.s_addr = gate;
			SAVE_SET_STRING(gw_info.ip, inet_ntoa(ina))
		}
	}
	fclose(fp);
	return 0;
}

void free_gateway_info(void)
{
	if (gw_info.iface)
		free(gw_info.iface);
	if (gw_info.ip)
		free(gw_info.ip);
	memset(&gw_info, 0, sizeof(gw_info));
}

int gateway_exists(void)
{
	return !!gw_info.count;
}

void print_gateway_iface(char *p, int p_max_size)
{
	snprintf(p, p_max_size, "%s", gw_info.iface);
}

void print_gateway_ip(char *p, int p_max_size)
{
	snprintf(p, p_max_size, "%s", gw_info.ip);
}

int update_net_stats(void)
{
	FILE *net_dev_fp;
	static int rep = 0;
	static char first = 1;

	// FIXME: arbitrary size chosen to keep code simple.
	int i, i2;
	unsigned int curtmp1, curtmp2;
	unsigned int k;
	struct ifconf conf;
	char buf[256];
	double delta;

#ifdef HAVE_IWLIB
	// wireless info variables
	int skfd, has_bitrate = 0;
	struct wireless_info *winfo;
	struct iwreq wrq;
#endif

	/* get delta */
	delta = current_update_time - last_update_time;
	if (delta <= 0.0001) {
		return 0;
	}

	/* open file and ignore first two lines */
	if (!(net_dev_fp = open_file("/proc/net/dev", &rep))) {
		clear_net_stats();
		return 0;
	}

	fgets(buf, 255, net_dev_fp);	/* garbage */
	fgets(buf, 255, net_dev_fp);	/* garbage (field names) */

	/* read each interface */
	for (i2 = 0; i2 < MAX_NET_INTERFACES; i2++) {
		struct net_stat *ns;
		char *s, *p;
		char temp_addr[18];
		long long r, t, last_recv, last_trans;

		if (fgets(buf, 255, net_dev_fp) == NULL) {
			break;
		}
		p = buf;
		while (isspace((int) *p)) {
			p++;
		}

		s = p;

		while (*p && *p != ':') {
			p++;
		}
		if (*p == '\0') {
			continue;
		}
		*p = '\0';
		p++;

		ns = get_net_stat(s, NULL, NULL);
		ns->up = 1;
		memset(&(ns->addr.sa_data), 0, 14);

		memset(ns->addrs, 0, 17 * MAX_NET_INTERFACES + 1); /* Up to 17 chars per ip, max MAX_NET_INTERFACES interfaces. Nasty memory usage... */

		last_recv = ns->recv;
		last_trans = ns->trans;

		/* bytes packets errs drop fifo frame compressed multicast|bytes ... */
		sscanf(p, "%lld  %*d     %*d  %*d  %*d  %*d   %*d        %*d       %lld",
			&r, &t);

		/* if recv or trans is less than last time, an overflow happened */
		if (r < ns->last_read_recv) {
			last_recv = 0;
		} else {
			ns->recv += (r - ns->last_read_recv);
		}
		ns->last_read_recv = r;

		if (t < ns->last_read_trans) {
			last_trans = 0;
		} else {
			ns->trans += (t - ns->last_read_trans);
		}
		ns->last_read_trans = t;

		/*** ip addr patch ***/
		i = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);

		conf.ifc_buf = malloc(sizeof(struct ifreq) * MAX_NET_INTERFACES);
		conf.ifc_len = sizeof(struct ifreq) * MAX_NET_INTERFACES;
		memset(conf.ifc_buf, 0, conf.ifc_len);

		ioctl((long) i, SIOCGIFCONF, &conf);

		for (k = 0; k < conf.ifc_len / sizeof(struct ifreq); k++) {
			struct net_stat *ns2;

			if (!(((struct ifreq *) conf.ifc_buf) + k))
				break;

			ns2 = get_net_stat(
					((struct ifreq *) conf.ifc_buf)[k].ifr_ifrn.ifrn_name, NULL, NULL);
			ns2->addr = ((struct ifreq *) conf.ifc_buf)[k].ifr_ifru.ifru_addr;
			sprintf(temp_addr, "%u.%u.%u.%u, ",
					ns2->addr.sa_data[2] & 255,
					ns2->addr.sa_data[3] & 255,
					ns2->addr.sa_data[4] & 255,
					ns2->addr.sa_data[5] & 255);
			if(NULL == strstr(ns2->addrs, temp_addr))
				strncpy(ns2->addrs + strlen(ns2->addrs), temp_addr, 17);
		}

		close((long) i);

		free(conf.ifc_buf);

		/*** end ip addr patch ***/

		if (!first) {
			/* calculate speeds */
			ns->net_rec[0] = (ns->recv - last_recv) / delta;
			ns->net_trans[0] = (ns->trans - last_trans) / delta;
		}

		curtmp1 = 0;
		curtmp2 = 0;
		// get an average
#ifdef HAVE_OPENMP
#pragma omp parallel for reduction(+:curtmp1, curtmp2) schedule(dynamic,10)
#endif /* HAVE_OPENMP */
		for (i = 0; i < info.net_avg_samples; i++) {
			curtmp1 = curtmp1 + ns->net_rec[i];
			curtmp2 = curtmp2 + ns->net_trans[i];
		}
		ns->recv_speed = curtmp1 / (double) info.net_avg_samples;
		ns->trans_speed = curtmp2 / (double) info.net_avg_samples;
		if (info.net_avg_samples > 1) {
#ifdef HAVE_OPENMP
#pragma omp parallel for schedule(dynamic,10)
#endif /* HAVE_OPENMP */
			for (i = info.net_avg_samples; i > 1; i--) {
				ns->net_rec[i - 1] = ns->net_rec[i - 2];
				ns->net_trans[i - 1] = ns->net_trans[i - 2];
			}
		}

#ifdef HAVE_IWLIB
		/* update wireless info */
		winfo = malloc(sizeof(struct wireless_info));
		memset(winfo, 0, sizeof(struct wireless_info));

		skfd = iw_sockets_open();
		if (iw_get_basic_config(skfd, s, &(winfo->b)) > -1) {

			// set present winfo variables
			if (iw_get_stats(skfd, s, &(winfo->stats),
					&winfo->range, winfo->has_range) >= 0) {
				winfo->has_stats = 1;
			}
			if (iw_get_range_info(skfd, s, &(winfo->range)) >= 0) {
				winfo->has_range = 1;
			}
			if (iw_get_ext(skfd, s, SIOCGIWAP, &wrq) >= 0) {
				winfo->has_ap_addr = 1;
				memcpy(&(winfo->ap_addr), &(wrq.u.ap_addr), sizeof(sockaddr));
			}

			// get bitrate
			if (iw_get_ext(skfd, s, SIOCGIWRATE, &wrq) >= 0) {
				memcpy(&(winfo->bitrate), &(wrq.u.bitrate), sizeof(iwparam));
				iw_print_bitrate(ns->bitrate, 16, winfo->bitrate.value);
				has_bitrate = 1;
			}

			// get link quality
			if (winfo->has_range && winfo->has_stats
					&& ((winfo->stats.qual.level != 0)
					|| (winfo->stats.qual.updated & IW_QUAL_DBM))) {
				if (!(winfo->stats.qual.updated & IW_QUAL_QUAL_INVALID)) {
					ns->link_qual = winfo->stats.qual.qual;
					ns->link_qual_max = winfo->range.max_qual.qual;
				}
			}

			// get ap mac
			if (winfo->has_ap_addr) {
				iw_sawap_ntop(&winfo->ap_addr, ns->ap);
			}

			// get essid
			if (winfo->b.has_essid) {
				if (winfo->b.essid_on) {
					snprintf(ns->essid, 32, "%s", winfo->b.essid);
				} else {
					snprintf(ns->essid, 32, "off/any");
				}
			}

			snprintf(ns->mode, 16, "%s", iw_operation_mode[winfo->b.mode]);
		}
		iw_sockets_close(skfd);
		free(winfo);
#endif
	}
	first = 0;

	fclose(net_dev_fp);
	return 0;
}

int result;

int update_total_processes(void)
{
	DIR *dir;
	struct dirent *entry;
	int ignore1;
	char ignore2;

	info.procs = 0;
	if (!(dir = opendir("/proc"))) {
		return 0;
	}
	while ((entry = readdir(dir))) {
		if (!entry) {
			/* Problem reading list of processes */
			closedir(dir);
			info.procs = 0;
			return 0;
		}
		if (sscanf(entry->d_name, "%d%c", &ignore1, &ignore2) == 1) {
			info.procs++;
		}
	}
	closedir(dir);
	return 0;
}

int update_threads(void)
{
#ifdef HAVE_SYSINFO
	if (!prefer_proc) {
		struct sysinfo s_info;

		sysinfo(&s_info);
		info.threads = s_info.procs;
	} else
#endif
	{
		static int rep = 0;
		FILE *fp;

		if (!(fp = open_file("/proc/loadavg", &rep))) {
			info.threads = 0;
			return 0;
		}
		fscanf(fp, "%*f %*f %*f %*d/%hu", &info.threads);
		fclose(fp);
	}
	return 0;
}

#define CPU_SAMPLE_COUNT 15
struct cpu_info {
	unsigned long long cpu_user;
	unsigned long long cpu_system;
	unsigned long long cpu_nice;
	unsigned long long cpu_idle;
	unsigned long long cpu_iowait;
	unsigned long long cpu_irq;
	unsigned long long cpu_softirq;
	unsigned long long cpu_steal;
	unsigned long long cpu_total;
	unsigned long long cpu_active_total;
	unsigned long long cpu_last_total;
	unsigned long long cpu_last_active_total;
	double cpu_val[CPU_SAMPLE_COUNT];
};
static short cpu_setup = 0;

/* Determine if this kernel gives us "extended" statistics information in
 * /proc/stat.
 * Kernels around 2.5 and earlier only reported user, system, nice, and
 * idle values in proc stat.
 * Kernels around 2.6 and greater report these PLUS iowait, irq, softirq,
 * and steal */
void determine_longstat(char *buf)
{
	unsigned long long iowait = 0;

	KFLAG_SETOFF(KFLAG_IS_LONGSTAT);
	/* scanf will either return -1 or 1 because there is only 1 assignment */
	if (sscanf(buf, "%*s %*d %*d %*d %*d %llu", &iowait) > 0) {
		KFLAG_SETON(KFLAG_IS_LONGSTAT);
	}
}

void get_cpu_count(void)
{
	FILE *stat_fp;
	static int rep = 0;
	char buf[256];

	if (info.cpu_usage) {
		return;
	}

	if (!(stat_fp = open_file("/proc/stat", &rep))) {
		return;
	}

	info.cpu_count = 0;

	while (!feof(stat_fp)) {
		if (fgets(buf, 255, stat_fp) == NULL) {
			break;
		}

		if (strncmp(buf, "cpu", 3) == 0 && isdigit(buf[3])) {
			if (info.cpu_count == 0) {
				determine_longstat(buf);
			}
			info.cpu_count++;
		}
	}
	info.cpu_usage = malloc((info.cpu_count + 1) * sizeof(float));

	fclose(stat_fp);
}

#define TMPL_LONGSTAT "%*s %llu %llu %llu %llu %llu %llu %llu %llu"
#define TMPL_SHORTSTAT "%*s %llu %llu %llu %llu"

int update_stat(void)
{
	FILE *stat_fp;
	static int rep = 0;
	static struct cpu_info *cpu = NULL;
	char buf[256];
	int i;
	unsigned int idx;
	double curtmp;
	const char *stat_template = NULL;
	unsigned int malloc_cpu_size = 0;
	extern void* global_cpu;

	static pthread_mutex_t last_stat_update_mutex = PTHREAD_MUTEX_INITIALIZER;
	static double last_stat_update = 0.0;

	/* since we use wrappers for this function, the update machinery
	 * can't eliminate double invocations of this function. Check for
	 * them here, otherwise cpu_usage counters are freaking out. */
	pthread_mutex_lock(&last_stat_update_mutex);
	if (last_stat_update == current_update_time) {
		pthread_mutex_unlock(&last_stat_update_mutex);
		return 0;
	}
	last_stat_update = current_update_time;
	pthread_mutex_unlock(&last_stat_update_mutex);

	/* add check for !info.cpu_usage since that mem is freed on a SIGUSR1 */
	if (!cpu_setup || !info.cpu_usage) {
		get_cpu_count();
		cpu_setup = 1;
	}

	if (!stat_template) {
		stat_template =
			KFLAG_ISSET(KFLAG_IS_LONGSTAT) ? TMPL_LONGSTAT : TMPL_SHORTSTAT;
	}

	if (!global_cpu) {
		malloc_cpu_size = (info.cpu_count + 1) * sizeof(struct cpu_info);
		cpu = malloc(malloc_cpu_size);
		memset(cpu, 0, malloc_cpu_size);
		global_cpu = cpu;
	}

	if (!(stat_fp = open_file("/proc/stat", &rep))) {
		info.run_threads = 0;
		if (info.cpu_usage) {
			memset(info.cpu_usage, 0, info.cpu_count * sizeof(float));
		}
		return 0;
	}

	idx = 0;
	while (!feof(stat_fp)) {
		if (fgets(buf, 255, stat_fp) == NULL) {
			break;
		}

		if (strncmp(buf, "procs_running ", 14) == 0) {
			sscanf(buf, "%*s %hu", &info.run_threads);
		} else if (strncmp(buf, "cpu", 3) == 0) {
			double delta;
			if (isdigit(buf[3])) {
				idx = atoi(&buf[3]) + 1;
			} else {
				idx = 0;
			}
			sscanf(buf, stat_template, &(cpu[idx].cpu_user),
				&(cpu[idx].cpu_nice), &(cpu[idx].cpu_system),
				&(cpu[idx].cpu_idle), &(cpu[idx].cpu_iowait),
				&(cpu[idx].cpu_irq), &(cpu[idx].cpu_softirq),
				&(cpu[idx].cpu_steal));

			cpu[idx].cpu_total = cpu[idx].cpu_user + cpu[idx].cpu_nice +
				cpu[idx].cpu_system + cpu[idx].cpu_idle +
				cpu[idx].cpu_iowait + cpu[idx].cpu_irq +
				cpu[idx].cpu_softirq + cpu[idx].cpu_steal;

			cpu[idx].cpu_active_total = cpu[idx].cpu_total -
				(cpu[idx].cpu_idle + cpu[idx].cpu_iowait);

			delta = current_update_time - last_update_time;

			if (delta <= 0.001) {
				break;
			}

			cpu[idx].cpu_val[0] = (cpu[idx].cpu_active_total -
				cpu[idx].cpu_last_active_total) /
				(float) (cpu[idx].cpu_total - cpu[idx].cpu_last_total);
			curtmp = 0;
#ifdef HAVE_OPENMP
#pragma omp parallel for reduction(+:curtmp) schedule(dynamic,10)
#endif /* HAVE_OPENMP */
			for (i = 0; i < info.cpu_avg_samples; i++) {
				curtmp = curtmp + cpu[idx].cpu_val[i];
			}
			/* TESTING -- I've removed this, because I don't think it is right.
			 * You shouldn't divide by the cpu count here ...
			 * removing for testing */
			/* if (idx == 0) {
				info.cpu_usage[idx] = curtmp / info.cpu_avg_samples /
					info.cpu_count;
			} else {
				info.cpu_usage[idx] = curtmp / info.cpu_avg_samples;
			} */
			/* TESTING -- this line replaces the prev. "suspect" if/else */
			info.cpu_usage[idx] = curtmp / info.cpu_avg_samples;

			cpu[idx].cpu_last_total = cpu[idx].cpu_total;
			cpu[idx].cpu_last_active_total = cpu[idx].cpu_active_total;
#ifdef HAVE_OPENMP
#pragma omp parallel for schedule(dynamic,10)
#endif /* HAVE_OPENMP */
			for (i = info.cpu_avg_samples - 1; i > 0; i--) {
				cpu[idx].cpu_val[i] = cpu[idx].cpu_val[i - 1];
			}
		}
	}
	fclose(stat_fp);
	return 0;
}

int update_running_processes(void)
{
	update_stat();
	return 0;
}

int update_cpu_usage(void)
{
	update_stat();
	return 0;
}

int update_load_average(void)
{
#ifdef HAVE_GETLOADAVG
	if (!prefer_proc) {
		double v[3];

		getloadavg(v, 3);
		info.loadavg[0] = (float) v[0];
		info.loadavg[1] = (float) v[1];
		info.loadavg[2] = (float) v[2];
	} else
#endif
	{
		static int rep = 0;
		FILE *fp;

		if (!(fp = open_file("/proc/loadavg", &rep))) {
			info.loadavg[0] = info.loadavg[1] = info.loadavg[2] = 0.0;
			return 0;
		}
		fscanf(fp, "%f %f %f", &info.loadavg[0], &info.loadavg[1],
			&info.loadavg[2]);
		fclose(fp);
	}
	return 0;
}

/***********************************************************/
/***********************************************************/
/***********************************************************/

static int no_dots(const struct dirent *d)
{
	if (d->d_name[0] == '.') {
		return 0;
	}
	return 1;
}

static int get_first_file_in_a_directory(const char *dir, char *s, int *rep)
{
	struct dirent **namelist;
	int i, n;

	n = scandir(dir, &namelist, no_dots, alphasort);
	if (n < 0) {
		if (!rep || !*rep) {
			NORM_ERR("scandir for %s: %s", dir, strerror(errno));
			if (rep) {
				*rep = 1;
			}
		}
		return 0;
	} else {
		if (n == 0) {
			return 0;
		}

		strncpy(s, namelist[0]->d_name, 255);
		s[255] = '\0';

#ifdef HAVE_OPENMP
#pragma omp parallel for schedule(dynamic,10)
#endif /* HAVE_OPENMP */
		for (i = 0; i < n; i++) {
			free(namelist[i]);
		}
		free(namelist);

		return 1;
	}
}

static int open_sysfs_sensor(const char *dir, const char *dev, const char *type, int n,
		int *divisor, char *devtype)
{
	char path[256];
	char buf[256];
	int fd;
	int divfd;

	memset(buf, 0, sizeof(buf));

	/* if device is NULL or *, get first */
	if (dev == NULL || strcmp(dev, "*") == 0) {
		static int rep = 0;

		if (!get_first_file_in_a_directory(dir, buf, &rep)) {
			return -1;
		}
		dev = buf;
	}

	if (strcmp(dir, "/sys/class/hwmon/") == 0) {
		if (*buf) {
			/* buf holds result from get_first_file_in_a_directory() above,
			 * e.g. "hwmon0" -- append "/device" */
			strcat(buf, "/device");
		} else {
			/* dev holds device number N as a string,
			 * e.g. "0", -- convert to "hwmon0/device" */
			sprintf(buf, "hwmon%s/device", dev);
			dev = buf;
		}
	}

	/* change vol to in, tempf to temp */
	if (strcmp(type, "vol") == 0) {
		type = "in";
	} else if (strcmp(type, "tempf") == 0) {
		type = "temp";
	}

	/* construct path */
	snprintf(path, 255, "%s%s/%s%d_input", dir, dev, type, n);

	/* first, attempt to open file in /device */
	fd = open(path, O_RDONLY);
	if (fd < 0) {

		/* if it fails, strip the /device from dev and attempt again */
		buf[strlen(buf) - 7] = 0;
		snprintf(path, 255, "%s%s/%s%d_input", dir, dev, type, n);
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			CRIT_ERR(NULL, NULL, "can't open '%s': %s\nplease check your device or remove this "
					 "var from "PACKAGE_NAME, path, strerror(errno));
		}
	}

	strncpy(devtype, path, 255);

	if (strcmp(type, "in") == 0 || strcmp(type, "temp") == 0
			|| strcmp(type, "tempf") == 0) {
		*divisor = 1;
	} else {
		*divisor = 0;
	}
	/* fan does not use *_div as a read divisor */
	if (strcmp("fan", type) == 0) {
		return fd;
	}

	/* test if *_div file exist, open it and use it as divisor */
	if (strcmp(type, "tempf") == 0) {
		snprintf(path, 255, "%s%s/%s%d_div", dir, "one", "two", n);
	} else {
		snprintf(path, 255, "%s%s/%s%d_div", dir, dev, type, n);
	}

	divfd = open(path, O_RDONLY);
	if (divfd > 0) {
		/* read integer */
		char divbuf[64];
		int divn;

		divn = read(divfd, divbuf, 63);
		/* should read until n == 0 but I doubt that kernel will give these
		 * in multiple pieces. :) */
		if (divn < 0) {
			NORM_ERR("open_sysfs_sensor(): can't read from sysfs");
		} else {
			divbuf[divn] = '\0';
			*divisor = atoi(divbuf);
		}
		close(divfd);
	}

	return fd;
}

static double get_sysfs_info(int *fd, int divisor, char *devtype, char *type)
{
	int val = 0;

	if (*fd <= 0) {
		return 0;
	}

	lseek(*fd, 0, SEEK_SET);

	/* read integer */
	{
		char buf[64];
		int n;
		n = read(*fd, buf, 63);
		/* should read until n == 0 but I doubt that kernel will give these
		 * in multiple pieces. :) */
		if (n < 0) {
			NORM_ERR("get_sysfs_info(): read from %s failed\n", devtype);
		} else {
			buf[n] = '\0';
			val = atoi(buf);
		}
	}

	close(*fd);
	/* open file */
	*fd = open(devtype, O_RDONLY);
	if (*fd < 0) {
		NORM_ERR("can't open '%s': %s", devtype, strerror(errno));
	}

	/* My dirty hack for computing CPU value
	 * Filedil, from forums.gentoo.org */
	/* if (strstr(devtype, "temp1_input") != NULL) {
		return -15.096 + 1.4893 * (val / 1000.0);
	} */

	/* divide voltage and temperature by 1000 */
	/* or if any other divisor is given, use that */
	if (strcmp(type, "tempf") == 0) {
		if (divisor > 1) {
			return ((val / divisor + 40) * 9.0 / 5) - 40;
		} else if (divisor) {
			return ((val / 1000.0 + 40) * 9.0 / 5) - 40;
		} else {
			return ((val + 40) * 9.0 / 5) - 40;
		}
	} else {
		if (divisor > 1) {
			return val / divisor;
		} else if (divisor) {
			return val / 1000.0;
		} else {
			return val;
		}
	}
}

#define HWMON_RESET() {\
		buf1[0] = 0; \
		factor = 1.0; \
		offset = 0.0; }

static void parse_sysfs_sensor(struct text_object *obj, const char *arg, const char *path, const char *type)
{
	char buf1[64], buf2[64];
	float factor, offset;
	int n, found = 0;
	struct sysfs *sf;

	if (sscanf(arg, "%63s %d %f %f", buf2, &n, &factor, &offset) == 4) found = 1; else HWMON_RESET();
	if (!found && sscanf(arg, "%63s %63s %d %f %f", buf1, buf2, &n, &factor, &offset) == 5) found = 1; else if (!found) HWMON_RESET();
	if (!found && sscanf(arg, "%63s %63s %d", buf1, buf2, &n) == 3) found = 1; else if (!found) HWMON_RESET();
	if (!found && sscanf(arg, "%63s %d", buf2, &n) == 2) found = 1; else if (!found) HWMON_RESET();

	if (!found) {
		NORM_ERR("i2c failed to parse arguments");
		obj->type = OBJ_text;
		return;
	}
	DBGP("parsed %s args: '%s' '%s' %d %f %f\n", type, buf1, buf2, n, factor, offset);
	sf = malloc(sizeof(struct sysfs));
	memset(sf, 0, sizeof(struct sysfs));
	sf->fd = open_sysfs_sensor(path, (*buf1) ? buf1 : 0, buf2, n,
			&sf->arg, sf->devtype);
	strncpy(sf->type, buf2, 63);
	sf->factor = factor;
	sf->offset = offset;
	obj->data.opaque = sf;
}

#define PARSER_GENERATOR(name, path)                                \
void parse_##name##_sensor(struct text_object *obj, const char *arg) \
{                                                                   \
	parse_sysfs_sensor(obj, arg, path, #name);           \
}

PARSER_GENERATOR(i2c, "/sys/bus/i2c/devices/")
PARSER_GENERATOR(hwmon, "/sys/class/hwmon/")
PARSER_GENERATOR(platform, "/sys/bus/platform/devices/")

void print_sysfs_sensor(struct text_object *obj, char *p, int p_max_size)
{
	double r;
	struct sysfs *sf = obj->data.opaque;

	if (!sf)
		return;

	r = get_sysfs_info(&sf->fd, sf->arg,
			sf->devtype, sf->type);

	r = r * sf->factor + sf->offset;

	if (!strncmp(sf->type, "temp", 4)) {
		temp_print(p, p_max_size, r, TEMP_CELSIUS);
	} else if (r >= 100.0 || r == 0) {
		snprintf(p, p_max_size, "%d", (int) r);
	} else {
		snprintf(p, p_max_size, "%.1f", r);
	}
}

void free_sysfs_sensor(struct text_object *obj)
{
	struct sysfs *sf = obj->data.opaque;

	if (!sf)
		return;

	close(sf->fd);
	free(obj->data.opaque);
	obj->data.opaque = NULL;
}

#define CPUFREQ_PREFIX "/sys/devices/system/cpu"
#define CPUFREQ_POSTFIX "cpufreq/scaling_cur_freq"

/* return system frequency in MHz (use divisor=1) or GHz (use divisor=1000) */
char get_freq(char *p_client_buffer, size_t client_buffer_size,
		const char *p_format, int divisor, unsigned int cpu)
{
	FILE *f;
	static int rep = 0;
	char frequency[32];
	char s[256];
	double freq = 0;

	if (!p_client_buffer || client_buffer_size <= 0 || !p_format
			|| divisor <= 0) {
		return 0;
	}

	if (!prefer_proc) {
		char current_freq_file[128];

		snprintf(current_freq_file, 127, "%s/cpu%d/%s", CPUFREQ_PREFIX, cpu - 1,
			CPUFREQ_POSTFIX);
		f = fopen(current_freq_file, "r");
		if (f) {
			/* if there's a cpufreq /sys node, read the current frequency from
			 * this node and divide by 1000 to get Mhz. */
			if (fgets(s, sizeof(s), f)) {
				s[strlen(s) - 1] = '\0';
				freq = strtod(s, NULL);
			}
			fclose(f);
			snprintf(p_client_buffer, client_buffer_size, p_format,
				(freq / 1000) / divisor);
			return 1;
		}
	}

	// open the CPU information file
	f = open_file("/proc/cpuinfo", &rep);
	if (!f) {
		perror(PACKAGE_NAME": Failed to access '/proc/cpuinfo' at get_freq()");
		return 0;
	}

	// read the file
	while (fgets(s, sizeof(s), f) != NULL) {

#if defined(__i386) || defined(__x86_64)
		// and search for the cpu mhz
		if (strncmp(s, "cpu MHz", 7) == 0 && cpu == 0) {
#else
#if defined(__alpha)
		// different on alpha
		if (strncmp(s, "cycle frequency [Hz]", 20) == 0 && cpu == 0) {
#else
		// this is different on ppc for some reason
		if (strncmp(s, "clock", 5) == 0 && cpu == 0) {
#endif // defined(__alpha)
#endif // defined(__i386) || defined(__x86_64)

			// copy just the number
			strcpy(frequency, strchr(s, ':') + 2);
#if defined(__alpha)
			// strip " est.\n"
			frequency[strlen(frequency) - 6] = '\0';
			// kernel reports in Hz
			freq = strtod(frequency, NULL) / 1000000;
#else
			// strip \n
			frequency[strlen(frequency) - 1] = '\0';
			freq = strtod(frequency, NULL);
#endif
			break;
		}
		if (strncmp(s, "processor", 9) == 0) {
			cpu--;
			continue;
		}
	}

	fclose(f);
	snprintf(p_client_buffer, client_buffer_size, p_format,
		(float) freq / divisor);
	return 1;
}

#define CPUFREQ_VOLTAGE "cpufreq/scaling_voltages"

/* /sys/devices/system/cpu/cpu0/cpufreq/scaling_voltages looks something
 * like this:
# frequency voltage
1800000 1340
1600000 1292
1400000 1100
1200000 988
1000000 1116
800000 1004
600000 988
 * Peter Tarjan (ptarjan@citromail.hu) */

/* return cpu voltage in mV (use divisor=1) or V (use divisor=1000) */
static char get_voltage(char *p_client_buffer, size_t client_buffer_size,
		const char *p_format, int divisor, unsigned int cpu)
{
	FILE *f;
	char s[256];
	int freq = 0;
	int voltage = 0;
	char current_freq_file[128];
	int freq_comp = 0;

	/* build the voltage file name */
	cpu--;
	snprintf(current_freq_file, 127, "%s/cpu%d/%s", CPUFREQ_PREFIX, cpu,
		CPUFREQ_POSTFIX);

	if (!p_client_buffer || client_buffer_size <= 0 || !p_format
			|| divisor <= 0) {
		return 0;
	}

	/* read the current cpu frequency from the /sys node */
	f = fopen(current_freq_file, "r");
	if (f) {
		if (fgets(s, sizeof(s), f)) {
			s[strlen(s) - 1] = '\0';
			freq = strtod(s, NULL);
		}
		fclose(f);
	} else {
		fprintf(stderr, PACKAGE_NAME": Failed to access '%s' at ", current_freq_file);
		perror("get_voltage()");
		if (f) {
			fclose(f);
		}
		return 0;
	}

	snprintf(current_freq_file, 127, "%s/cpu%d/%s", CPUFREQ_PREFIX, cpu,
		CPUFREQ_VOLTAGE);

	/* use the current cpu frequency to find the corresponding voltage */
	f = fopen(current_freq_file, "r");

	if (f) {
		while (!feof(f)) {
			char line[256];

			if (fgets(line, 255, f) == NULL) {
				break;
			}
			sscanf(line, "%d %d", &freq_comp, &voltage);
			if (freq_comp == freq) {
				break;
			}
		}
		fclose(f);
	} else {
		fprintf(stderr, PACKAGE_NAME": Failed to access '%s' at ", current_freq_file);
		perror("get_voltage()");
		if (f) {
			fclose(f);
		}
		return 0;
	}
	snprintf(p_client_buffer, client_buffer_size, p_format,
		(float) voltage / divisor);
	return 1;
}

void print_voltage_mv(struct text_object *obj, char *p, int p_max_size)
{
	static int ok = 1;
	if (ok) {
		ok = get_voltage(p, p_max_size, "%.0f", 1, obj->data.i);
	}
}

void print_voltage_v(struct text_object *obj, char *p, int p_max_size)
{
	static int ok = 1;
	if (ok) {
		ok = get_voltage(p, p_max_size, "%'.3f", 1000, obj->data.i);
	}
}

#define ACPI_FAN_DIR "/proc/acpi/fan/"

void get_acpi_fan(char *p_client_buffer, size_t client_buffer_size)
{
	static int rep = 0;
	char buf[256];
	char buf2[256];
	FILE *fp;

	if (!p_client_buffer || client_buffer_size <= 0) {
		return;
	}

	/* yeah, slow... :/ */
	if (!get_first_file_in_a_directory(ACPI_FAN_DIR, buf, &rep)) {
		snprintf(p_client_buffer, client_buffer_size, "no fans?");
		return;
	}

	snprintf(buf2, sizeof(buf2), "%s%s/state", ACPI_FAN_DIR, buf);

	fp = open_file(buf2, &rep);
	if (!fp) {
		snprintf(p_client_buffer, client_buffer_size,
			"can't open fan's state file");
		return;
	}
	memset(buf, 0, sizeof(buf));
	fscanf(fp, "%*s %99s", buf);
	fclose(fp);

	snprintf(p_client_buffer, client_buffer_size, "%s", buf);
}

#define SYSFS_AC_ADAPTER_DIR "/sys/class/power_supply"
#define ACPI_AC_ADAPTER_DIR "/proc/acpi/ac_adapter/"
/* Linux 2.6.25 onwards ac adapter info is in
   /sys/class/power_supply/AC/
   On my system I get the following.
     /sys/class/power_supply/AC/uevent:
     PHYSDEVPATH=/devices/LNXSYSTM:00/device:00/PNP0A08:00/device:01/PNP0C09:00/ACPI0003:00
     PHYSDEVBUS=acpi
     PHYSDEVDRIVER=ac
     POWER_SUPPLY_NAME=AC
     POWER_SUPPLY_TYPE=Mains
     POWER_SUPPLY_ONLINE=1

   Update: it seems the folder name is hardware-dependent. We add an aditional adapter
   argument, specifying the folder name.

   Update: on some systems it's /sys/class/power_supply/ADP1 instead of /sys/class/power_supply/AC
*/

void get_acpi_ac_adapter(char *p_client_buffer, size_t client_buffer_size, const char *adapter)
{
	static int rep = 0;

	char buf[256];
	char buf2[256];
	struct stat sb;
	FILE *fp;

	if (!p_client_buffer || client_buffer_size <= 0) {
		return;
	}

	if(adapter)
		snprintf(buf2, sizeof(buf2), "%s/%s/uevent", SYSFS_AC_ADAPTER_DIR, adapter);
	else{
		snprintf(buf2, sizeof(buf2), "%s/AC/uevent", SYSFS_AC_ADAPTER_DIR);
		if(stat(buf2, &sb) == -1) snprintf(buf2, sizeof(buf2), "%s/ADP1/uevent", SYSFS_AC_ADAPTER_DIR);
	}
	if(stat(buf2, &sb) == 0) fp = open_file(buf2, &rep); else fp = 0;
	if (fp) {
		/* sysfs processing */
		while (!feof(fp)) {
			if (fgets(buf, sizeof(buf), fp) == NULL)
				break;

			if (strncmp(buf, "POWER_SUPPLY_ONLINE=", 20) == 0) {
				int online = 0;
				sscanf(buf, "POWER_SUPPLY_ONLINE=%d", &online);
				snprintf(p_client_buffer, client_buffer_size,
					 "%s-line", (online ? "on" : "off"));
				break;
			}
		}
		fclose(fp);
	} else {
		/* yeah, slow... :/ */
		if (!get_first_file_in_a_directory(ACPI_AC_ADAPTER_DIR, buf, &rep)) {
			snprintf(p_client_buffer, client_buffer_size, "no ac_adapters?");
			return;
		}

		snprintf(buf2, sizeof(buf2), "%s%s/state", ACPI_AC_ADAPTER_DIR, buf);

		fp = open_file(buf2, &rep);
		if (!fp) {
			snprintf(p_client_buffer, client_buffer_size,
				 "No ac adapter found.... where is it?");
			return;
		}
		memset(buf, 0, sizeof(buf));
		fscanf(fp, "%*s %99s", buf);
		fclose(fp);

		snprintf(p_client_buffer, client_buffer_size, "%s", buf);
	}
}

/*
/proc/acpi/thermal_zone/THRM/cooling_mode
cooling mode:            active
/proc/acpi/thermal_zone/THRM/polling_frequency
<polling disabled>
/proc/acpi/thermal_zone/THRM/state
state:                   ok
/proc/acpi/thermal_zone/THRM/temperature
temperature:             45 C
/proc/acpi/thermal_zone/THRM/trip_points
critical (S5):           73 C
passive:                 73 C: tc1=4 tc2=3 tsp=40 devices=0xcdf6e6c0
*/

#define ACPI_THERMAL_DIR "/proc/acpi/thermal_zone/"
#define ACPI_THERMAL_FORMAT "/proc/acpi/thermal_zone/%s/temperature"

int open_acpi_temperature(const char *name)
{
	char path[256];
	char buf[256];
	int fd;

	if (name == NULL || strcmp(name, "*") == 0) {
		static int rep = 0;

		if (!get_first_file_in_a_directory(ACPI_THERMAL_DIR, buf, &rep)) {
			return -1;
		}
		name = buf;
	}

	snprintf(path, 255, ACPI_THERMAL_FORMAT, name);

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		NORM_ERR("can't open '%s': %s", path, strerror(errno));
	}

	return fd;
}

static double last_acpi_temp;
static double last_acpi_temp_time;

double get_acpi_temperature(int fd)
{
	if (fd <= 0) {
		return 0;
	}

	/* don't update acpi temperature too often */
	if (current_update_time - last_acpi_temp_time < 11.32) {
		return last_acpi_temp;
	}
	last_acpi_temp_time = current_update_time;

	/* seek to beginning */
	lseek(fd, 0, SEEK_SET);

	/* read */
	{
		char buf[256];
		int n;

		n = read(fd, buf, 255);
		if (n < 0) {
			NORM_ERR("can't read fd %d: %s", fd, strerror(errno));
		} else {
			buf[n] = '\0';
			sscanf(buf, "temperature: %lf", &last_acpi_temp);
		}
	}

	return last_acpi_temp;
}

/*
hipo@lepakko hipo $ cat /proc/acpi/battery/BAT1/info
present:                 yes
design capacity:         4400 mAh
last full capacity:      4064 mAh
battery technology:      rechargeable
design voltage:          14800 mV
design capacity warning: 300 mAh
design capacity low:     200 mAh
capacity granularity 1:  32 mAh
capacity granularity 2:  32 mAh
model number:            02KT
serial number:           16922
battery type:            LION
OEM info:                SANYO
*/

/*
hipo@lepakko conky $ cat /proc/acpi/battery/BAT1/state
present:                 yes
capacity state:          ok
charging state:          unknown
present rate:            0 mA
remaining capacity:      4064 mAh
present voltage:         16608 mV
*/

/*
2213<@jupet�kellari��> jupet@lagi-unstable:~$ cat /proc/apm
2213<@jupet�kellari��> 1.16 1.2 0x03 0x01 0xff 0x10 -1% -1 ?
2213<@jupet�kellari��> (-1 ollee ei akkua kiinni, koska akku on p�yd�ll�)
2214<@jupet�kellari��> jupet@lagi-unstable:~$ cat /proc/apm
2214<@jupet�kellari��> 1.16 1.2 0x03 0x01 0x03 0x09 98% -1 ?

2238<@jupet�kellari��> 1.16 1.2 0x03 0x00 0x00 0x01 100% -1 ? ilman verkkovirtaa
2239<@jupet�kellari��> 1.16 1.2 0x03 0x01 0x00 0x01 99% -1 ? verkkovirralla

2240<@jupet�kellari��> 1.16 1.2 0x03 0x01 0x03 0x09 100% -1 ? verkkovirralla ja monitori p��ll�
2241<@jupet�kellari��> 1.16 1.2 0x03 0x00 0x00 0x01 99% -1 ? monitori p��ll� mutta ilman verkkovirtaa
*/

/* Kapil Hari Paranjape <kapil@imsc.res.in>
  Linux 2.6.24 onwards battery info is in
  /sys/class/power_supply/BAT0/
  On my system I get the following.
	/sys/class/power_supply/BAT0/uevent:
	PHYSDEVPATH=/devices/LNXSYSTM:00/device:00/PNP0A03:00/device:01/PNP0C09:00/PNP0C0A:00
	PHYSDEVBUS=acpi
	PHYSDEVDRIVER=battery
	POWER_SUPPLY_NAME=BAT0
	POWER_SUPPLY_TYPE=Battery
	POWER_SUPPLY_STATUS=Discharging
	POWER_SUPPLY_PRESENT=1
	POWER_SUPPLY_TECHNOLOGY=Li-ion
	POWER_SUPPLY_VOLTAGE_MIN_DESIGN=10800000
	POWER_SUPPLY_VOLTAGE_NOW=10780000
	POWER_SUPPLY_CURRENT_NOW=13970000
	POWER_SUPPLY_ENERGY_FULL_DESIGN=47510000
	POWER_SUPPLY_ENERGY_FULL=27370000
	POWER_SUPPLY_ENERGY_NOW=11810000
	POWER_SUPPLY_MODEL_NAME=IBM-92P1060
	POWER_SUPPLY_MANUFACTURER=Panasonic
  On some systems POWER_SUPPLY_ENERGY_* is replaced by POWER_SUPPLY_CHARGE_*
*/

/*on n900 with power kernel: (non power kernel could use "hal-device bme" and do something else)
/sys/class/power_supply/bq27200-0/uevent
PHYSDEVPATH=/class/i2c-adapter/i2c-2/2-0055
PHYSDEVBUS=i2c
PHYSDEVDRIVER=bq27200-battery
POWER_SUPPLY_NAME=bq27200-0
POWER_SUPPLY_TYPE=Battery
POWER_SUPPLY_PRESENT=1				<< this is always 1, it means the battery is inserted
POWER_SUPPLY_VOLTAGE_NOW=4039		<< this keeps updating during charging, both here and in dbus
POWER_SUPPLY_CURRENT_NOW=1960       << this goes negative when charging!
POWER_SUPPLY_CAPACITY=98			<< supposed to be the %, it keeps updating while charging, unlike the dbus vals that freeze
POWER_SUPPLY_TEMP=39				<< only temperature sensor in n900 :(
*/

#define SYSFS_BATTERY_BASE_PATH "/sys/class/power_supply"
#define ACPI_BATTERY_BASE_PATH "/proc/acpi/battery" //not for n900
#define APM_PATH "/proc/apm"  //not for n900
#define MAX_BATTERY_COUNT 4 //more like 1

static FILE *sysfs_bat_fp[MAX_BATTERY_COUNT] = { NULL, NULL, NULL, NULL };
//static FILE *acpi_bat_fp[MAX_BATTERY_COUNT] = { NULL, NULL, NULL, NULL };
//static FILE *apm_bat_fp[MAX_BATTERY_COUNT] = { NULL, NULL, NULL, NULL };

static int batteries_initialized = 0;
static char batteries[MAX_BATTERY_COUNT][32];

//static int acpi_last_full[MAX_BATTERY_COUNT];
//static int acpi_design_capacity[MAX_BATTERY_COUNT];

//eg 4100
static int last_battery_volts[MAX_BATTERY_COUNT];

//eg -78
static dbus_int32_t last_cell_radio_dbm;

//eg 100
static dbus_int32_t last_cell_radio_percent;

//eg 'full' 'on' 'off'
static char last_batt_charge_status[16];

//eg 100 or -100
static int last_battery_rate[MAX_BATTERY_COUNT];

//eg 35.5
static float last_battery_temp[MAX_BATTERY_COUNT];

/* e.g. "charging 75%" */
static char last_battery_str[MAX_BATTERY_COUNT][64];
/* e.g. "3h 15m" */
static char last_battery_time_str[MAX_BATTERY_COUNT][64];

static double last_battery_time[MAX_BATTERY_COUNT];

static int last_battery_perct[MAX_BATTERY_COUNT];
static double last_battery_perct_time[MAX_BATTERY_COUNT];

void init_batteries(void)
{
	int idx;

	if (batteries_initialized) {
		return;
	}
#ifdef HAVE_OPENMP
#pragma omp parallel for schedule(dynamic,10)
#endif /* HAVE_OPENMP */
	for (idx = 0; idx < MAX_BATTERY_COUNT; idx++) {
		batteries[idx][0] = '\0';
	}
	batteries_initialized = 1;
}

int get_battery_idx(const char *bat)
{
//	int idx;
//
//	for (idx = 0; idx < MAX_BATTERY_COUNT; idx++) {
//		if (!strlen(batteries[idx]) || !strcmp(batteries[idx], bat)) {
//			break;
//		}
//	}
//
//	/* if not found, enter a new entry */
//	if (!strlen(batteries[idx])) {
//		snprintf(batteries[idx], 31, "%s", bat);
//	}

	return 0;
}

static int dbus_queue = 0;

void set_dbus_retval(char *buffer, unsigned int n, int item);

DBusConnection *connection;
DBusError error;
DBusMessage *message;
DBusMessageIter iter;
DBusBusType type;
int message_type;
DBusMessage *reply;

void dbus_exit_app_view()
{
	//not needed in harmattan - swipe down or away and close (although it doesn't seem to be reliable to swipe)
	
	
	//dbus-send --type=signal --session	/com/nokia/hildon_desktop .

//	type = DBUS_BUS_SESSION;
//	//message_type = DBUS_MESSAGE_TYPE_SIGNAL;
//	dbus_error_init (&error);
//	connection = dbus_bus_get (type, &error);
//    message = dbus_message_new_signal ("/com/nokia/hildon_desktop", "com.nokia.hildon_desktop", "exit_app_view");
//    // send the message and flush the connection
//    if (!dbus_connection_send(connection, message, NULL)) {
//       fprintf(stderr, "Out Of Memory!\n");
//       exit(1);
//    }
//    dbus_connection_flush(connection);
//
//    // free the message
//    dbus_message_unref(message);

}

void get_dbus_stuff(char *buffer,unsigned int intMax_length, int item)
{
	char method[128];
	char path[128];
	char dest[128];
	char *args = "";
	char *args2 = "";
	if (dbus_queue > 10)
	{
		fprintf (stderr, "too much dbus queuing\n");
		exit(1);
//		set_dbus_retval(buffer, intMax_length, item);
//		return;
	}
	dbus_queue++;//prevent a queue from forming on these requests...
//fetch data from dbus, store in here as last_cell_radio_dbm
//return into buffer

	type = DBUS_BUS_SYSTEM;
	message_type = DBUS_MESSAGE_TYPE_METHOD_CALL;
//	print_reply = TRUE;
//	print_reply_literal = FALSE;
	int reply_timeout_ms = 5000;
	dbus_error_init (&error);
	connection = dbus_bus_get (type, &error);
	if (connection == NULL)
    {
		fprintf (stderr, "Failed to open connection to %s message bus: %s\n",
               (type == DBUS_BUS_SYSTEM) ? "system" : "session",
               error.message);
      dbus_error_free (&error);
      exit (1);
    }
	switch(item){
	case DBUS_CELL_DBM:		
		snprintf(method,127,"Get");
		args = "com.nokia.csd.CSNet.SignalStrength";
		args2 = "SignalDecibels";
		snprintf(path,127,"/com/nokia/csd/csnet");
		snprintf(dest,127,"com.nokia.csd.CSNet"); //service name
		message = dbus_message_new_method_call (dest,path,"org.freedesktop.DBus.Properties",method);//dest,path,interface,method
		dbus_message_set_auto_start (message, TRUE);
		if (!dbus_message_append_args(message,
			DBUS_TYPE_STRING, &args,
			DBUS_TYPE_STRING, &args2,
			DBUS_TYPE_INVALID))
				fprintf (stderr, "OOM appending args\n");
		//dbus_message_iter_init_append(message, &iter);
		//if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &args))
		//	fprintf (stderr, "OOM appending args\n");
		break;
	case DBUS_CELL_PERCENT:
		snprintf(method,127,"Get");
		args = "com.nokia.csd.CSNet.SignalStrength";
		args2 = "SignalPercent";
		snprintf(path,127,"/com/nokia/csd/csnet");
		snprintf(dest,127,"com.nokia.csd.CSNet"); //service name
		message = dbus_message_new_method_call (dest,path,"org.freedesktop.DBus.Properties",method);//dest,path,interface,method
		dbus_message_set_auto_start (message, TRUE);
		if (!dbus_message_append_args(message,
			DBUS_TYPE_STRING, &args,
			DBUS_TYPE_STRING, &args2,
			DBUS_TYPE_INVALID))
				fprintf (stderr, "OOM appending args\n");
		//dbus_message_iter_init_append(message, &iter);
		//if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &args))
		//	fprintf (stderr, "OOM appending args\n");
		break;
	case DBUS_HAL_BATTERY_CHRG_STATUS:
		// 'full' 'on' 'off'
		snprintf(method,127,"GetProperty");
		args = "maemo.rechargeable.charging_status";
		snprintf(path,127,"/org/freedesktop/Hal/devices/bme");
		snprintf(dest,127,"org.freedesktop.Hal");
		message = dbus_message_new_method_call (dest,path,"org.freedesktop.Hal.Device",method);
		dbus_message_set_auto_start (message, TRUE);
		dbus_message_iter_init_append(message, &iter);
		if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &args))
			fprintf (stderr, "OOM appending args\n");
		break;
	case DBUS_HAL_BATTERY_PERCENT:
		// '96'
		snprintf(method,127,"GetProperty");
		args = "battery.charge_level.percentage";
		snprintf(path,127,"/org/freedesktop/Hal/devices/bme");
		snprintf(dest,127,"org.freedesktop.Hal");
		message = dbus_message_new_method_call (dest,path,"org.freedesktop.Hal.Device",method);
		dbus_message_set_auto_start (message, TRUE);
		dbus_message_iter_init_append(message, &iter);
		if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &args))
			fprintf (stderr, "OOM appending args\n");
		break;
	case DBUS_HAL_BATTERY_VOLTS_CURRENT:
		// '3600' - '4200'
		snprintf(method,127,"GetProperty");
		args = "battery.voltage.current"; //battery.reporting.current gets battery mA, not charge/discharge rate
		snprintf(path,127,"/org/freedesktop/Hal/devices/bme");
		snprintf(dest,127,"org.freedesktop.Hal");
		message = dbus_message_new_method_call (dest,path,"org.freedesktop.Hal.Device",method);
		dbus_message_set_auto_start (message, TRUE);
		dbus_message_iter_init_append(message, &iter);
		if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &args))
			fprintf (stderr, "OOM appending args\n");
		break;
	default:
		fprintf (stderr, "invalid item type in get_dbus_stuff");
		break;
	}
	if (message == NULL)
	{
	  fprintf (stderr, "Couldn't allocate D-Bus message\n");
	  exit (1);
	}
	if (!dbus_message_set_destination (message, dest))
	{
	  fprintf (stderr, "Not enough memory\n");
	  exit (1);
	}

	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (connection, message, reply_timeout_ms, &error);
	if (dbus_error_is_set (&error))
	{
	  fprintf (stderr, "Error %s: %s\n",error.name,error.message);
	  //exit (1);//if we set timeout to 30s or something i guess it's okay to exit on "no reply" cuz something is fu*ked;
	}
	if (reply)
	{
		DBusMessageIter iter;
		DBusMessageIter subiter;
		dbus_message_iter_init (reply, &iter);
		int current_fieldnumber = 0;
		while (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID)
		{
			
			current_fieldnumber++;
			switch(item){
			case DBUS_CELL_DBM:
				if (current_fieldnumber == 1)
				{//this is a variant
					if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT){
						fprintf (stderr,"DBUS_CELL_DBM got type '%c'; expected variant!\n", dbus_message_iter_get_arg_type(&iter));
						break;
					}
					dbus_message_iter_recurse (&iter, &subiter);				
					dbus_int32_t val = 0;
					if (dbus_message_iter_get_arg_type(&subiter) != DBUS_TYPE_INT32){
						fprintf (stderr,"DBUS_CELL_DBM subiter got type '%c'; expected INT32!\n", dbus_message_iter_get_arg_type(&subiter));
						break;
					}
					dbus_message_iter_get_basic(&subiter, &val);
					last_cell_radio_dbm = val;
				}
				break;
			case DBUS_CELL_PERCENT:
				if (current_fieldnumber == 1)
				{//this is a variant
					dbus_message_iter_recurse (&iter, &subiter);
					dbus_int32_t val = 0;
					dbus_message_iter_get_basic(&subiter, &val);
					last_cell_radio_percent = val;
				}
				break;
			case DBUS_HAL_BATTERY_CHRG_STATUS:
				if (current_fieldnumber == 1)
				{
					char *val;
					dbus_message_iter_get_basic(&iter, &val);
					snprintf(last_batt_charge_status,16,"%s",val);
				}
				break;
			case DBUS_HAL_BATTERY_PERCENT:
				if (current_fieldnumber == 1)
				{
					dbus_uint32_t val;
					dbus_message_iter_get_basic(&iter, &val);
					last_battery_perct[0] = val;
				}
				break;
			case DBUS_HAL_BATTERY_VOLTS_CURRENT:
				if (current_fieldnumber == 1)
				{
					dbus_uint32_t val;
					dbus_message_iter_get_basic(&iter, &val);
					last_battery_volts[0] = val;
				}
				break;
			default:
				fprintf (stderr, "invalid item type in get_dbus_stuff reply loop");
				break;
			}
			dbus_message_iter_next (&iter);
		}
		dbus_message_unref (reply);
	}
	set_dbus_retval(buffer, intMax_length, item);
	dbus_message_unref (message);
	dbus_connection_unref (connection);
	dbus_queue = 0;//reset to zero now that complete
}

void set_dbus_retval(char *buffer, unsigned int intMax_length, int item)
{
	switch (item) {
		case DBUS_CELL_DBM:
			snprintf(buffer, intMax_length, "%d", last_cell_radio_dbm);
			break;
		case DBUS_CELL_PERCENT:
			snprintf(buffer, intMax_length, "%d", last_cell_radio_percent);
			break;
		case DBUS_HAL_BATTERY_CHRG_STATUS:
			snprintf(buffer, intMax_length, "%s", last_batt_charge_status);
			break;
		case DBUS_HAL_BATTERY_PERCENT:
			snprintf(buffer, intMax_length, "%i", last_battery_perct[0]);
			break;
		case DBUS_HAL_BATTERY_VOLTS_CURRENT:
			snprintf(buffer, intMax_length, "%i", last_battery_volts[0]);
			break;
		default:
			fprintf (stderr, "invalid item type in set_dbus_retval");
			break;
	}
}

void set_return_value(char *buffer, unsigned int n, int item, int idx);

void get_battery_stuff(char *buffer, unsigned int n, const char *bat, int item)
{
	static int idx, rep = 0;
	char acpi_path[128];
	char sysfs_path[128];

	snprintf(acpi_path, 127, ACPI_BATTERY_BASE_PATH "/%s/state", bat);
	snprintf(sysfs_path, 127, SYSFS_BATTERY_BASE_PATH "/%s/uevent", bat);

	init_batteries();

	idx = get_battery_idx(bat);

	/* don't update battery too often */
	if (current_update_time - last_battery_time[idx] < 2) {
		set_return_value(buffer, n, item, idx);
		return;
	}

	last_battery_time[idx] = current_update_time;

	memset(last_battery_str[idx], 0, sizeof(last_battery_str[idx]));
	memset(last_battery_time_str[idx], 0, sizeof(last_battery_time_str[idx]));

 	/* first try SYSFS if that fails try DBUS */

 	if (sysfs_bat_fp[idx] == NULL) {
 		sysfs_bat_fp[idx] = open_file(sysfs_path, &rep);
	}

 	if (sysfs_bat_fp[idx] != NULL) {
 		/* SYSFS */
 		int present_rate = -9999; //we will put "current now" into this. negative when charging!
 		int remaining_capacity = -1; //in %
 		char charging_state[64];//can't get this without hal bme
 		int voltage = -1;
 		int temp = 9999;
 		char present[4];
 		strcpy(present, "no");
 		strcpy(charging_state, "unknown");

 		while (!feof(sysfs_bat_fp[idx])) {
 			//we are looping through the data here
 			char buf[256];
 			if (fgets(buf, 256, sysfs_bat_fp[idx]) == NULL)
 				break;
 			if (strncmp (buf, "POWER_SUPPLY_PRESENT=1", 22) == 0)//does current row match this?
 				strcpy(present, "yes");//n900 always yes
 			else if (strncmp(buf, "POWER_SUPPLY_VOLTAGE_NOW=", 25) == 0)//keep checking possible matches
 				sscanf(buf, "POWER_SUPPLY_VOLTAGE_NOW=%d", &voltage);
 			else if (strncmp(buf, "POWER_SUPPLY_CURRENT_NOW=", 25) == 0)
 				sscanf(buf, "POWER_SUPPLY_CURRENT_NOW=%d", &present_rate);
 			else if (strncmp(buf, "POWER_SUPPLY_CAPACITY=", 22) == 0)
 				sscanf(buf, "POWER_SUPPLY_CAPACITY=%d", &remaining_capacity);
 			else if (strncmp(buf, "POWER_SUPPLY_TEMP=", 18) == 0)
 				sscanf(buf, "POWER_SUPPLY_TEMP=%d", &temp);
 		}
 		fclose(sysfs_bat_fp[idx]);
 		sysfs_bat_fp[idx] = NULL;
		if (voltage > 10000) voltage = voltage / 1000;  //fix for n900 power kernel 47
		last_battery_volts[idx] = voltage;
 		if (temp < 100)
 			last_battery_temp[idx] = temp;
 		else //fix for n900 power kernel 47
 			last_battery_temp[idx] = (float) temp / 10;
 		/* charging */
 		if (present_rate <= 0) {
 				/* e.g. charging 75% */
 				snprintf(last_battery_str[idx], sizeof(last_battery_str[idx])-1, "charging %i%%", remaining_capacity);
 				snprintf(last_battery_time_str[idx], sizeof(last_battery_time_str[idx]) - 1, "unknown");
		}
 		/* discharging */
 		else if (present_rate > 0) {
 				/* e.g. discharging 35% */
 				snprintf(last_battery_str[idx], sizeof(last_battery_str[idx])-1, "discharging %i%%", remaining_capacity);
 				snprintf(last_battery_time_str[idx], sizeof(last_battery_time_str[idx]) - 1, "unknown");
 		}
 		/* charged */
 		 if (remaining_capacity == 100)
 				strcpy(last_battery_str[idx], "charged 100%");
	} else {
		//if don't have power kernel, use HAL / dbus
 		int remaining_capacity = -1; //in %
 		int temp = -1;
 		remaining_capacity = get_battery_perct(bat);
 		get_dbus_stuff(buffer,n,DBUS_HAL_BATTERY_VOLTS_CURRENT);
 		get_dbus_stuff(buffer,n,DBUS_HAL_BATTERY_CHRG_STATUS);
 		if (strncmp(buffer, "on", 2) == 0) {
			snprintf(last_battery_str[idx], sizeof(last_battery_str[idx])-1, "charging %i%%", remaining_capacity);
 		}
 		else if (strncmp(buffer, "off", 3) == 0) {
 			snprintf(last_battery_str[idx], sizeof(last_battery_str[idx])-1, "discharging %i%%", remaining_capacity);
 		}
 		else if (strncmp(buffer, "full", 4) == 0) {//no, it won't always be 100%. stupid dbus.
 			snprintf(last_battery_str[idx], sizeof(last_battery_str[idx])-1, "charged %i%%", remaining_capacity);
 		 }
                
                struct bme_reply bmeInfo = getBattInfoFromBME();
                last_battery_temp[idx] = ((float)bmeInfo.battery_temperature) - 273.15f;
                last_battery_rate[idx] = bmeInfo.battery_current;
 		//last_battery_temp[idx] = temp;                
	}
	set_return_value(buffer, n, item, idx);
}

void set_return_value(char *buffer, unsigned int n, int item, int idx)
{
	switch (item) {
		case BATTERY_STATUS:
			snprintf(buffer, n, "%s", last_battery_str[idx]);
			break;
		case BATTERY_TIME:
			snprintf(buffer, n, "%s", last_battery_time_str[idx]);
			break;
		case BATTERY_VOLTS:
			snprintf(buffer, n, "%i", last_battery_volts[idx]); // voltage
			break;
		case BATTERY_TEMP:
			snprintf(buffer, n, "%3.1f", last_battery_temp[idx]); // temperature
			break;
		case BATTERY_RATE:
			snprintf(buffer, n, "%i", last_battery_rate[idx]); // charge/discharge rate
			break;                  
		default:
			fprintf (stderr, "invalid item type in set_return_value");
			break;
	}
}

void get_battery_short_status(char *buffer, unsigned int n, const char *bat)
{
	get_battery_stuff(buffer, n, bat, BATTERY_STATUS);
	if (0 == strncmp("charging", buffer, 8)) {
		buffer[0] = 'C';
		memmove(buffer + 1, buffer + 8, n - 8);
	} else if (0 == strncmp("discharging", buffer, 11)) {
		buffer[0] = 'D';
		memmove(buffer + 1, buffer + 11, n - 11);
	} else if (0 == strncmp("charged", buffer, 7)) {
		buffer[0] = 'F';
		memmove(buffer + 1, buffer + 7, n - 7);
	} else if (0 == strncmp("not present", buffer, 11)) {
		buffer[0] = 'N';
		memmove(buffer + 1, buffer + 11, n - 11);
	} else if (0 == strncmp("empty", buffer, 5)) {
		buffer[0] = 'E';
		memmove(buffer + 1, buffer + 5, n - 5);
	} else if (0 != strncmp("AC", buffer, 2)) {
		buffer[0] = 'U';
		memmove(buffer + 1, buffer + 11, n - 11);
	} else fprintf (stderr, "invalid input buffer in get_battery_short_status");
}

int get_battery_perct(const char *bat)
{
	static int rep = 0;
	int idx;
	char sysfs_path[128];
	int remaining_capacity = -1;
	snprintf(sysfs_path, 127, SYSFS_BATTERY_BASE_PATH "/%s/uevent", bat);

	init_batteries();
	idx = get_battery_idx(bat);

	/* don't update battery too often */
	if (current_update_time - last_battery_perct_time[idx] < 29.5) {
		return last_battery_perct[idx];
	}
	last_battery_perct_time[idx] = current_update_time;

	/* try  SYSFS first */
	sysfs_bat_fp[idx] = open_file(sysfs_path, &rep);
	if (sysfs_bat_fp[idx] != NULL) {
		/* SYSFS */
		while (!feof(sysfs_bat_fp[idx])) {
			char buf[256];
			if (fgets(buf, 256, sysfs_bat_fp[idx]) == NULL)
				break;
			if (strncmp(buf, "POWER_SUPPLY_CAPACITY=", 22) == 0) {
				sscanf(buf, "POWER_SUPPLY_CAPACITY=%d", &remaining_capacity);

//			if (strncmp(buf, "POWER_SUPPLY_CHARGE_NOW=", 24) == 0) {
//				sscanf(buf, "POWER_SUPPLY_CHARGE_NOW=%d", &remaining_capacity);
//			} else if (strncmp(buf, "POWER_SUPPLY_CHARGE_FULL=",25) == 0) {
//				sscanf(buf, "POWER_SUPPLY_CHARGE_FULL=%d", &acpi_design_capacity[idx]);
//			} else if (strncmp(buf, "POWER_SUPPLY_ENERGY_NOW=", 24) == 0) {
//				sscanf(buf, "POWER_SUPPLY_ENERGY_NOW=%d", &remaining_capacity);
//			} else if (strncmp(buf, "POWER_SUPPLY_ENERGY_FULL=",25) == 0) {
//				sscanf(buf, "POWER_SUPPLY_ENERGY_FULL=%d", &acpi_design_capacity[idx]);
//			}
			}
		}
		fclose(sysfs_bat_fp[idx]);
		sysfs_bat_fp[idx] = NULL;
		last_battery_perct[idx] = remaining_capacity;

	}
	else
	{
		char *useless;
		useless = malloc(128 * sizeof(char));
		get_dbus_stuff(useless,128,DBUS_HAL_BATTERY_PERCENT);
		//i told you it's useless...
		//snprintf(last_battery_perct[idx], 127, "%i", useless);
		//guess what? dbus battery % doesn't update while charging
//		get_dbus_stuff(useless,n,DBUS_HAL_BATTERY_CHRG_STATUS);
//		if (strncmp(useless, "full", 4) == 0)
//		{
//
//		}
	}
//	else if (acpi_bat_fp[idx] != NULL) {
//		/* ACPI */
//		/* read last full capacity if it's zero */
//		if (acpi_design_capacity[idx] == 0) {
//			static int rep2;
//			char path[128];
//			FILE *fp;
//
//			snprintf(path, 127, ACPI_BATTERY_BASE_PATH "/%s/info", bat);
//			fp = open_file(path, &rep2);
//			if (fp != NULL) {
//				while (!feof(fp)) {
//					char b[256];
//
//					if (fgets(b, 256, fp) == NULL) {
//						break;
//					}
//					if (sscanf(b, "last full capacity: %d",
//								&acpi_design_capacity[idx]) != 0) {
//						break;
//					}
//				}
//				fclose(fp);
//			}
//		}
//
//		fseek(acpi_bat_fp[idx], 0, SEEK_SET);
//
//		while (!feof(acpi_bat_fp[idx])) {
//			char buf[256];
//
//			if (fgets(buf, 256, acpi_bat_fp[idx]) == NULL) {
//				break;
//			}
//
//			if (buf[0] == 'r') {
//				sscanf(buf, "remaining capacity: %d", &remaining_capacity);
//			}
//		}
//	}

	/* compute the battery percentage */

		//(int) (((float) remaining_capacity / acpi_design_capacity[idx]) * 100);
	//if (last_battery_perct[idx] > 100) last_battery_perct[idx] = 100;
	return last_battery_perct[idx];
}

int get_battery_perct_bar(const char *bat)
{
	int idx;

	get_battery_perct(bat);
	idx = get_battery_idx(bat);
	return (int) (last_battery_perct[idx] * 2.56 - 1);
}

/* On Apple powerbook and ibook:
$ cat /proc/pmu/battery_0
flags      : 00000013
charge     : 3623
max_charge : 3720
current    : 388
voltage    : 16787
time rem.  : 900
$ cat /proc/pmu/info
PMU driver version     : 2
PMU firmware version   : 0c
AC Power               : 1
Battery count          : 1
*/

/* defines as in <linux/pmu.h> */
#define PMU_BATT_PRESENT		0x00000001
#define PMU_BATT_CHARGING		0x00000002

static FILE *pmu_battery_fp;
static FILE *pmu_info_fp;
static char pb_battery_info[3][32];
static double pb_battery_info_update;

#define PMU_PATH "/proc/pmu"
void get_powerbook_batt_info(char *buffer, size_t n, int i)
{
	static int rep = 0;
	const char *batt_path = PMU_PATH "/battery_0";
	const char *info_path = PMU_PATH "/info";
	unsigned int flags;
	int charge, max_charge, ac = -1;
	long timeval = -1;

	/* don't update battery too often */
	if (current_update_time - pb_battery_info_update < 29.5) {
		snprintf(buffer, n, "%s", pb_battery_info[i]);
		return;
	}
	pb_battery_info_update = current_update_time;

	if (pmu_battery_fp == NULL) {
		pmu_battery_fp = open_file(batt_path, &rep);
		if (pmu_battery_fp == NULL) {
			return;
		}
	}

	if (pmu_battery_fp != NULL) {
		rewind(pmu_battery_fp);
		while (!feof(pmu_battery_fp)) {
			char buf[32];

			if (fgets(buf, sizeof(buf), pmu_battery_fp) == NULL) {
				break;
			}

			if (buf[0] == 'f') {
				sscanf(buf, "flags      : %8x", &flags);
			} else if (buf[0] == 'c' && buf[1] == 'h') {
				sscanf(buf, "charge     : %d", &charge);
			} else if (buf[0] == 'm') {
				sscanf(buf, "max_charge : %d", &max_charge);
			} else if (buf[0] == 't') {
				sscanf(buf, "time rem.  : %ld", &timeval);
			}
		}
	}
	if (pmu_info_fp == NULL) {
		pmu_info_fp = open_file(info_path, &rep);
		if (pmu_info_fp == NULL) {
			return;
		}
	}

	if (pmu_info_fp != NULL) {
		rewind(pmu_info_fp);
		while (!feof(pmu_info_fp)) {
			char buf[32];

			if (fgets(buf, sizeof(buf), pmu_info_fp) == NULL) {
				break;
			}
			if (buf[0] == 'A') {
				sscanf(buf, "AC Power               : %d", &ac);
			}
		}
	}
	/* update status string */
	if ((ac && !(flags & PMU_BATT_PRESENT))) {
		strncpy(pb_battery_info[PB_BATT_STATUS], "AC", sizeof(pb_battery_info[PB_BATT_STATUS]));
	} else if (ac && (flags & PMU_BATT_PRESENT)
			&& !(flags & PMU_BATT_CHARGING)) {
		strncpy(pb_battery_info[PB_BATT_STATUS], "charged", sizeof(pb_battery_info[PB_BATT_STATUS]));
	} else if ((flags & PMU_BATT_PRESENT) && (flags & PMU_BATT_CHARGING)) {
		strncpy(pb_battery_info[PB_BATT_STATUS], "charging", sizeof(pb_battery_info[PB_BATT_STATUS]));
	} else {
		strncpy(pb_battery_info[PB_BATT_STATUS], "discharging", sizeof(pb_battery_info[PB_BATT_STATUS]));
	}

	/* update percentage string */
	if (timeval == 0 && ac && (flags & PMU_BATT_PRESENT)
			&& !(flags & PMU_BATT_CHARGING)) {
		snprintf(pb_battery_info[PB_BATT_PERCENT],
			sizeof(pb_battery_info[PB_BATT_PERCENT]), "100%%");
	} else if (timeval == 0) {
		snprintf(pb_battery_info[PB_BATT_PERCENT],
			sizeof(pb_battery_info[PB_BATT_PERCENT]), "unknown");
	} else {
		snprintf(pb_battery_info[PB_BATT_PERCENT],
			sizeof(pb_battery_info[PB_BATT_PERCENT]), "%d%%",
			(charge * 100) / max_charge);
	}

	/* update time string */
	if (timeval == 0) {			/* fully charged or battery not present */
		snprintf(pb_battery_info[PB_BATT_TIME],
			sizeof(pb_battery_info[PB_BATT_TIME]), "unknown");
	} else if (timeval < 60 * 60) {	/* don't show secs */
		format_seconds_short(pb_battery_info[PB_BATT_TIME],
			sizeof(pb_battery_info[PB_BATT_TIME]), timeval);
	} else {
		format_seconds(pb_battery_info[PB_BATT_TIME],
			sizeof(pb_battery_info[PB_BATT_TIME]), timeval);
	}

	snprintf(buffer, n, "%s", pb_battery_info[i]);
}

int update_top(void)
{
	process_find_top(info.cpu, info.memu, info.time
#ifdef IOSTATS
                , info.io
#endif
                );
	info.first_process = get_first_process();
	return 0;
}

#define ENTROPY_AVAIL_PATH "/proc/sys/kernel/random/entropy_avail"

int get_entropy_avail(unsigned int *val)
{
	static int rep = 0;
	FILE *fp;

	if (!(fp = open_file(ENTROPY_AVAIL_PATH, &rep)))
		return 1;

	if (fscanf(fp, "%u", val) != 1)
		return 1;

	fclose(fp);
	return 0;
}

#define ENTROPY_POOLSIZE_PATH "/proc/sys/kernel/random/poolsize"

int get_entropy_poolsize(unsigned int *val)
{
	static int rep = 0;
	FILE *fp;

	if (!(fp = open_file(ENTROPY_POOLSIZE_PATH, &rep)))
		return 1;

	if (fscanf(fp, "%u", val) != 1)
		return 1;

	fclose(fp);
	return 0;
}

const char *get_disk_protect_queue(const char *disk)
{
	FILE *fp;
	char path[128];
	int state;

	snprintf(path, 127, "/sys/block/%s/device/unload_heads", disk);
	if (access(path, F_OK)) {
		snprintf(path, 127, "/sys/block/%s/queue/protect", disk);
	}
	if ((fp = fopen(path, "r")) == NULL)
		return "n/a   ";
	if (fscanf(fp, "%d\n", &state) != 1) {
		fclose(fp);
		return "failed";
	}
	fclose(fp);
	return (state > 0) ? "frozen" : "free  ";
}

typedef struct DEV_LIST_TYPE
{
	char *dev_name;
	int memoized;
	struct DEV_LIST_TYPE *next;

} DEV_LIST, *DEV_LIST_PTR;

/* Same as sf #2942117 but memoized using a linked list */
int is_disk(char *dev)
{
	char syspath[PATH_MAX];
	char *slash;
	static DEV_LIST_PTR dev_head = NULL;
	DEV_LIST_PTR dev_cur, dev_last;

	dev_cur = dev_head;

	while (dev_cur) {
		if (strcmp(dev_cur->dev_name, dev) == 0)
			return dev_cur->memoized;
		dev_last = dev_cur;
		dev_cur  = dev_cur->next;
	}

	dev_cur = (DEV_LIST_PTR)malloc(sizeof(DEV_LIST));
	dev_cur->dev_name = (char *)malloc((strlen(dev)+1)*sizeof(char));
	strcpy(dev_cur->dev_name,dev);
	dev_cur->next = NULL;

	while ((slash = strchr(dev, '/')))
		*slash = '!';
	snprintf(syspath, sizeof(syspath), "/sys/block/%s", dev);
	dev_cur->memoized = !(access(syspath, F_OK));

	if (dev_head)
		dev_last->next = dev_cur;
	else
		dev_head = dev_cur;

	return dev_cur->memoized;
}

int update_diskio(void)
{
	FILE *fp;
	static int rep = 0;
	char buf[512], devbuf[64];
	unsigned int major, minor;
	int col_count = 0;
	struct diskio_stat *cur;
	unsigned int reads, writes;
	unsigned int total_reads = 0, total_writes = 0;

	stats.current = 0;
	stats.current_read = 0;
	stats.current_write = 0;

	if (!(fp = open_file("/proc/diskstats", &rep))) {
		return 0;
	}

	/* read reads and writes from all disks (minor = 0), including cd-roms
	 * and floppies, and sum them up */
	while (fgets(buf, 512, fp)) {
		col_count = sscanf(buf, "%u %u %s %*u %*u %u %*u %*u %*u %u", &major,
			&minor, devbuf, &reads, &writes);
		/* ignore subdevices (they have only 3 matching entries in their line)
		 * and virtual devices (LVM, network block devices, RAM disks, Loopback)
		 *
		 * XXX: ignore devices which are part of a SW RAID (MD_MAJOR) */
		if (col_count == 5 && major != LVM_BLK_MAJOR && major != NBD_MAJOR
				&& major != RAMDISK_MAJOR && major != LOOP_MAJOR) {
			/* check needed for kernel >= 2.6.31, see sf #2942117 */
			if (is_disk(devbuf)) {
				total_reads += reads;
				total_writes += writes;
			}
		} else {
			col_count = sscanf(buf, "%u %u %s %*u %u %*u %u",
				&major, &minor, devbuf, &reads, &writes);
			if (col_count != 5) {
				continue;
			}
		}
		cur = stats.next;
		while (cur && strcmp(devbuf, cur->dev))
			cur = cur->next;

		if (cur)
			update_diskio_values(cur, reads, writes);
	}
	update_diskio_values(&stats, total_reads, total_writes);
	fclose(fp);
	return 0;
}
