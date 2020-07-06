/*-
 * Copyright (c) 2017 Domagoj Stolfa
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#ifndef WITHOUT_CAPSICUM
#include <sys/capsicum.h>
#endif
#include <sys/event.h>
#include <sys/uio.h>
#include <sys/uuid.h>
#include <sys/types.h>
#include <sys/proc.h>
#include <sys/ucred.h>
#include <sys/dtrace_bsd.h>
#include <sys/vtdtr.h>
#include <sys/stat.h>

#include <machine/vmm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <err.h>

#include <vmmapi.h>

#include "dthyve.h"
#include "bhyverun.h"
#include "pci_emul.h"
#include "virtio.h"

#define	VTDTR_RINGSZ	1024
#define	VTDTR_MAXQ	2

#define	VTDTR_DEVICE_READY		0x00 /* Device is ready */
#define	VTDTR_DEVICE_REGISTER		0x01 /* UNUSED */
#define	VTDTR_DEVICE_UNREGISTER		0x02 /* UNUSED */
#define	VTDTR_DEVICE_DESTROY		0x03 /* UNUSED */
#define	VTDTR_DEVICE_PROBE_CREATE	0x04 /* UNUSED */
#define	VTDTR_DEVICE_PROBE_INSTALL	0x05 /* Install a given probe */
#define	VTDTR_DEVICE_PROBE_UNINSTALL	0x06 /* Uninstall a given probe */
#define	VTDTR_DEVICE_EOF		0x07 /* End of file */
#define	VTDTR_DEVICE_GO			0x08 /* Start the tracing */
#define	VTDTR_DEVICE_ELF		0x09 /* Send an ELF file */
#define	VTDTR_DEVICE_STOP		0x0A /* Stop tracing */

static int pci_vtdtr_debug;
#define	DPRINTF(params) if (pci_vtdtr_debug) printf params
#define	WPRINTF(params) printf params

struct pci_vtdtr_control {
	uint32_t 		pvc_event;
	union {
		uint32_t	pvc_probeid;	/* install/uninstall event */

		struct {			/*  elf event */
			size_t	pvc_elflen;
			int	pvc_elfhasmore;
		} elf;

		/*
		 * Defines for easy access into the union and underlying structs
		 */
#define	pvc_probeid	uctrl.pvc_probeid

#define	pvc_elflen	uctrl.elf.pvc_elflen
#define	pvc_elfhasmore	uctrl.elf.pvc_elfhasmore
	} uctrl;

	/*
	 * Use the fact that a flexibly array member is not included in the
	 * struct's sizeof() to simply put it out of the union and simplify
	 * the code which uses it.
	 */
	char	pvc_elf[];
} __packed;

struct pci_vtdtr_ctrl_entry {
	STAILQ_ENTRY(pci_vtdtr_ctrl_entry)	entries;
	struct pci_vtdtr_control		*ctrl;
};

struct pci_vtdtr_ctrlq {
	STAILQ_HEAD(, pci_vtdtr_ctrl_entry)	head;
	pthread_mutex_t				mtx;
};

struct pci_vtdtr_softc {
	struct virtio_softc	vsd_vs;
	struct vqueue_info	vsd_queues[VTDTR_MAXQ];
	struct vmctx		*vsd_vmctx;
	struct pci_vtdtr_ctrlq	*vsd_ctrlq;
	pthread_mutex_t		vsd_condmtx;
	pthread_cond_t		vsd_cond;
	pthread_mutex_t		vsd_mtx;
	uint64_t		vsd_cfg;
	int			vsd_guest_ready;
	int			vsd_ready;
};

/*
 * By defn, a flexibly array member is not included in the sizeof(), so we can
 * simply compute the maximum amount of bytes we can fit in each event to not
 * overwrite the ringbuffer using it.
 */
#define	PCI_VTDTR_MAXELFSIZE	(VTDTR_RINGSZ - sizeof(struct pci_vtdtr_control))

static void pci_vtdtr_reset(void *);
static void pci_vtdtr_control_tx(struct pci_vtdtr_softc *,
    struct iovec *, int);
static int pci_vtdtr_control_rx(struct pci_vtdtr_softc *,
    struct iovec *, int);
static void pci_vtdtr_process_prov_evt(struct pci_vtdtr_softc *,
    struct pci_vtdtr_control *);
static void pci_vtdtr_process_probe_evt(struct pci_vtdtr_softc *,
    struct pci_vtdtr_control *);
static void pci_vtdtr_notify_tx(void *, struct vqueue_info *);
static void pci_vtdtr_notify_rx(void *, struct vqueue_info *);
static void pci_vtdtr_cq_enqueue(struct pci_vtdtr_ctrlq *,
    struct pci_vtdtr_ctrl_entry *);
static void pci_vtdtr_cq_enqueue_front(struct pci_vtdtr_ctrlq *,
    struct pci_vtdtr_ctrl_entry *);
static int pci_vtdtr_cq_empty(struct pci_vtdtr_ctrlq *);
static struct pci_vtdtr_ctrl_entry *pci_vtdtr_cq_dequeue(
    struct pci_vtdtr_ctrlq *);
static void pci_vtdtr_fill_desc(struct vqueue_info *,
    struct pci_vtdtr_control *);
static void pci_vtdtr_poll(struct vqueue_info *, int);
static void pci_vtdtr_notify_ready(struct pci_vtdtr_softc *);
static void pci_vtdtr_fill_eof_desc(struct vqueue_info *);
static void * pci_vtdtr_run(void *);
static void pci_vtdtr_reset_queue(struct pci_vtdtr_softc *);
static int pci_vtdtr_init(struct vmctx *, struct pci_devinst *, char *);

static struct virtio_consts vtdtr_vi_consts = {
	"vtdtr",			/* name */
	VTDTR_MAXQ,			/* maximum virtqueues */
	0,				/* config reg size */
	pci_vtdtr_reset,		/* reset */
	NULL,				/* device-wide qnotify */
	NULL,				/* read virtio config */
	NULL,				/* write virtio config */
	NULL,				/* apply negotiated features */
	0,				/* capabilities */
};

static void
pci_vtdtr_reset(void *xsc)
{
	struct pci_vtdtr_softc *sc;

	sc = xsc;

	pthread_mutex_lock(&sc->vsd_mtx);
	DPRINTF(("vtdtr: device reset requested!\n"));
	pci_vtdtr_reset_queue(sc);
	vi_reset_dev(&sc->vsd_vs);
	pthread_mutex_unlock(&sc->vsd_mtx);
}

static void
pci_vtdtr_control_tx(struct pci_vtdtr_softc *sc, struct iovec *iov, int niov)
{
	/*
	 * TODO
	 */
}

/*
 * In this function we process each of the events, for probe and provider
 * related events, we delegate the processing to a function specialized for that
 * type of event.
 */
static int
pci_vtdtr_control_rx(struct pci_vtdtr_softc *sc, struct iovec *iov, int niov)
{
	struct pci_vtdtr_control *ctrl;
	int retval;// error;

	assert(niov == 1);
	retval = 0;

	ctrl = (struct pci_vtdtr_control *)iov->iov_base;
	switch (ctrl->pvc_event) {
	case VTDTR_DEVICE_READY:
		pthread_mutex_lock(&sc->vsd_mtx);
		sc->vsd_guest_ready = 1;
		pthread_mutex_unlock(&sc->vsd_mtx);
		break;
	case VTDTR_DEVICE_EOF:
		retval = 1;
		break;
	default:
		WPRINTF(("Warning: Unknown event: %u\n", ctrl->pvc_event));
		break;
	}

	return (retval);
}

static void
pci_vtdtr_process_prov_evt(struct pci_vtdtr_softc *sc,
    struct pci_vtdtr_control *ctrl)
{
	/*
	 * XXX: The processing functions... are the actually
	 * necessary, or do we want a layer that DTrace talks
	 * to and simply delegates it towards the virtio driver?
	 */
}

static void
pci_vtdtr_process_probe_evt(struct pci_vtdtr_softc *sc,
    struct pci_vtdtr_control *ctrl)
{

}

static void
pci_vtdtr_notify_tx(void *xsc, struct vqueue_info *vq)
{
}

/*
 * The RX queue interrupt. This function gets all the descriptors until we hit
 * EOF or run out of descriptors and processes each event in a lockless manner.
 */
static void
pci_vtdtr_notify_rx(void *xsc, struct vqueue_info *vq)
{
	struct pci_vtdtr_softc *sc;
	struct iovec iov[1];
	uint16_t idx;
	uint16_t flags[8];
	int n;
	int retval;

	sc = xsc;

	while (vq_has_descs(vq)) {
		n = vq_getchain(vq, &idx, iov, 1, flags);
		retval = pci_vtdtr_control_rx(sc, iov, 1);
		vq_relchain(vq, idx, sizeof(struct pci_vtdtr_control));
		if (retval == 1)
			break;
	}

	pthread_mutex_lock(&sc->vsd_mtx);
	if (sc->vsd_ready == 0)
		pci_vtdtr_notify_ready(sc);
	pthread_mutex_unlock(&sc->vsd_mtx);

	pci_vtdtr_poll(vq, 1);

	pthread_mutex_lock(&sc->vsd_condmtx);
	pthread_cond_signal(&sc->vsd_cond);
	pthread_mutex_unlock(&sc->vsd_condmtx);

}

static __inline void
pci_vtdtr_cq_enqueue(struct pci_vtdtr_ctrlq *cq,
    struct pci_vtdtr_ctrl_entry *ctrl_entry)
{

	STAILQ_INSERT_TAIL(&cq->head, ctrl_entry, entries);
}

static __inline void
pci_vtdtr_cq_enqueue_front(struct pci_vtdtr_ctrlq *cq,
    struct pci_vtdtr_ctrl_entry *ctrl_entry)
{

	STAILQ_INSERT_HEAD(&cq->head, ctrl_entry, entries);
}

static __inline int
pci_vtdtr_cq_empty(struct pci_vtdtr_ctrlq *cq)
{

	return (STAILQ_EMPTY(&cq->head));
}

static struct pci_vtdtr_ctrl_entry *
pci_vtdtr_cq_dequeue(struct pci_vtdtr_ctrlq *cq)
{
	struct pci_vtdtr_ctrl_entry *ctrl_entry;
	ctrl_entry = STAILQ_FIRST(&cq->head);
	if (ctrl_entry != NULL) {
		STAILQ_REMOVE_HEAD(&cq->head, entries);
	}

	return (ctrl_entry);
}

/*
 * In this function we fill the descriptor that was provided to us by the guest.
 * No allocation is needed, since we memcpy everything.
 */
static void
pci_vtdtr_fill_desc(struct vqueue_info *vq, struct pci_vtdtr_control *ctrl)
{
	struct iovec iov;
	size_t len;
	int n;
	uint16_t idx;

	n = vq_getchain(vq, &idx, &iov, 1, NULL);
	assert(n == 1);

	len = sizeof(struct pci_vtdtr_control);
	memcpy(iov.iov_base, ctrl, len);

	vq_relchain(vq, idx, len);
}

static void
pci_vtdtr_poll(struct vqueue_info *vq, int all_used)
{

	vq_endchains(vq, all_used);
}

/*
 * In this function we enqueue the READY control message in front of the queue,
 * so that when the guest receives the messages, READY is the first one in the
 * queue. If we already are ready, we simply signal the communicator thread that
 * it is safe to run.
 */
static void
pci_vtdtr_notify_ready(struct pci_vtdtr_softc *sc)
{
	struct pci_vtdtr_ctrl_entry *ctrl_entry;
	struct pci_vtdtr_control *ctrl;

	sc->vsd_ready = 1;

	ctrl_entry = malloc(sizeof(struct pci_vtdtr_ctrl_entry));
	assert(ctrl_entry != NULL);
	memset(ctrl_entry, 0, sizeof(struct pci_vtdtr_ctrl_entry));

	ctrl = malloc(sizeof(struct pci_vtdtr_control));
	assert(ctrl != NULL);
	memset(ctrl, 0, sizeof(struct pci_vtdtr_control));

	ctrl->pvc_event = VTDTR_DEVICE_READY;
	ctrl_entry->ctrl = ctrl;

	pthread_mutex_lock(&sc->vsd_ctrlq->mtx);
	pci_vtdtr_cq_enqueue_front(sc->vsd_ctrlq, ctrl_entry);
	pthread_mutex_unlock(&sc->vsd_ctrlq->mtx);
}

static void
pci_vtdtr_fill_eof_desc(struct vqueue_info *vq)
{
	struct pci_vtdtr_control ctrl;
	ctrl.pvc_event = VTDTR_DEVICE_EOF;
	pci_vtdtr_fill_desc(vq, &ctrl);
}

/*
 * The communicator thread that is created when we attach the PCI device. It
 * serves the purpose of draining the control queue of messages and filling the
 * guest memory with the descriptors.
 */
static void *
pci_vtdtr_run(void *xsc)
{
	struct pci_vtdtr_softc *sc;
	struct pci_vtdtr_ctrl_entry *ctrl_entry;
	struct vqueue_info *vq;
	uint32_t nent;
	int error;
	int ready_flag;

	sc = xsc;
	vq = &sc->vsd_queues[0];

	for (;;) {
		nent = 0;
		error = 0;
		ready_flag = 1;

		error = pthread_mutex_lock(&sc->vsd_condmtx);
		assert(error == 0);
		/*
		 * We are safe to proceed if the following conditions are
		 * satisfied:
		 * (1) We have messages in the control queue
		 * (2) The guest is ready
		 */
		while (!sc->vsd_guest_ready ||
		    pci_vtdtr_cq_empty(sc->vsd_ctrlq)) {
			error = pthread_cond_wait(&sc->vsd_cond, &sc->vsd_condmtx);
			assert(error == 0);
		}
		error = pthread_mutex_unlock(&sc->vsd_condmtx);
		assert(error == 0);

		assert(vq_has_descs(vq) != 0);
		error = pthread_mutex_lock(&sc->vsd_ctrlq->mtx);
		assert(error == 0);
		assert(!pci_vtdtr_cq_empty(sc->vsd_ctrlq));

		/*
		 * While dealing with the entires, we will fill every single
		 * entry as long as we have space or entries in the queue.
		 */
		while (vq_has_descs(vq) && !pci_vtdtr_cq_empty(sc->vsd_ctrlq)) {
			ctrl_entry = pci_vtdtr_cq_dequeue(sc->vsd_ctrlq);
			error = pthread_mutex_unlock(&sc->vsd_ctrlq->mtx);
			assert(error == 0);

			if (ready_flag &&
			    ctrl_entry->ctrl->pvc_event != VTDTR_DEVICE_READY)
				ready_flag = 0;

			pci_vtdtr_fill_desc(vq, ctrl_entry->ctrl);
			free(ctrl_entry->ctrl);
			free(ctrl_entry);
			nent++;
			error = pthread_mutex_lock(&sc->vsd_ctrlq->mtx);
			assert(error == 0);
		}

		/*
		 * If we've filled >= 1 entry in the descriptor queue, we first
		 * check if the queue is empty, and if so, we append a special
		 * EOF descriptor to send to the guest. Following that, we end
		 * the chains and force an interrupt in the guest
		 */
		if (nent) {
			if (pci_vtdtr_cq_empty(sc->vsd_ctrlq) &&
			    vq_has_descs(vq)) {
				pci_vtdtr_fill_eof_desc(vq);
			}
			pthread_mutex_lock(&sc->vsd_mtx);
			sc->vsd_guest_ready = ready_flag;
			pthread_mutex_unlock(&sc->vsd_mtx);
			pci_vtdtr_poll(vq, 1);
		}

		error = pthread_mutex_unlock(&sc->vsd_ctrlq->mtx);

	}

	pthread_exit(NULL);
}

/*
 * A simple wrapper function used to reset the control queue
 */
static void
pci_vtdtr_reset_queue(struct pci_vtdtr_softc *sc)
{
	struct pci_vtdtr_ctrl_entry *n1, *n2;
	struct pci_vtdtr_ctrlq *q;

	q = sc->vsd_ctrlq;

	pthread_mutex_lock(&q->mtx);
	n1 = STAILQ_FIRST(&q->head);
	while (n1 != NULL) {
		n2 = STAILQ_NEXT(n1, entries);
		free(n1);
		n1 = n2;
	}

	STAILQ_INIT(&q->head);
	pthread_mutex_unlock(&q->mtx);
}

static int
pci_vtdtr_find(const char *vm,
    char vms[VTDTR_MAXVMS][VTDTR_VMNAMEMAX], size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (strcmp(vm, vms[i]) == 0)
			return (0);
	}

	return (-1);
}

static struct pci_vtdtr_control *
vtdtr_elf_event(int fd, size_t offs, struct stat *st)
{
	struct pci_vtdtr_control *ctrl;
	ssize_t rval;
	size_t maxlen;
	size_t len_to_read;
	void *buf;
	int hasmore;

	rval = 0;
	ctrl = NULL;
	maxlen = 0;
	len_to_read = 0;
	buf = NULL;
	hasmore = 0;

	assert(fd != -1);

	/*
	 * Sanity checks.
	 */
	if (st == NULL) {
		fprintf(stderr, "st is NULL\n");
		return (NULL);
	}

	/*
	 * Computer how much we'll actually be reading.
	 */
	maxlen = st->st_size - offs;
	len_to_read = maxlen > PCI_VTDTR_MAXELFSIZE ?
	    PCI_VTDTR_MAXELFSIZE : maxlen;
	hasmore = maxlen > PCI_VTDTR_MAXELFSIZE ? 1 : 0;

	/*
	 * Allocate the control message with the appropriate size to fit
	 * all of the data that we'll be reading in.
	 */
	ctrl = malloc(sizeof(struct pci_vtdtr_control) + len_to_read);
	if (ctrl == NULL) {
		fprintf(stderr, "failed to malloc a new control event\n");
		return (NULL);
	}

	/*
	 * Zero the control event.
	 */
	memset(ctrl, 0, sizeof(struct pci_vtdtr_control));

	/*
	 * We compute the offset to pvc_elf in the control message.
	 */
	buf = ctrl + offsetof(struct pci_vtdtr_control, pvc_elf);

	/*
	 * Read the actual data from the file.
	 */
	rval = pread(fd, buf, len_to_read, offs);
	if (rval != len_to_read) {
		fprintf(stderr, "failed to read from %d, got %zd bytes: %s\n",
		    fd, rval, strerror(errno));

		free(ctrl);
		return (NULL);
	}

	/*
	 * At this point, we will have returned NULL in any case of failure,
	 * so we don't need to check anything further and can fill in the full
	 * control message and return it.
	 */
	ctrl->pvc_event = VTDTR_DEVICE_ELF;
	ctrl->pvc_elflen = len_to_read;
	ctrl->pvc_elfhasmore = hasmore;

	return (ctrl);
}

static struct stat *
vtdtr_get_filestat(int fd)
{
	struct stat *st;
	int rval;

	st = NULL;
	rval = 0;

	st = malloc(sizeof(struct stat));
	assert(st != NULL);
	memset(st, 0, sizeof(struct stat));

	rval = fstat(fd, st);
	if (rval != 0) {
		fprintf(stderr, "failed to fstat: %s\n", strerror(errno));
		free(st);
		return (NULL);
	}

	return (st);
}

static void *
pci_vtdtr_events(void *xsc)
{
	struct pci_vtdtr_softc *sc;
	int error;
	size_t flags;
	const char *elfpath;
	int fd;
	struct stat *st;
	size_t offs;

	sc = xsc;
	fd = 0;
	st = NULL;
	offs = 0;

	DPRINTF(("%s: starting event reads.\n", __func__));

	/*
	 * We listen for events indefinitely.
	 */
	for (;;) {
		struct vtdtr_event ev;
		struct pci_vtdtr_ctrl_entry *ctrl_entry;
		struct pci_vtdtr_control *ctrl;

next:
		error = dthyve_read(&ev, 1);
		if (error) {
			fprintf(stderr, "Error: '%s' reading.\n",
			    strerror(error));
			if (errno == EINTR)
			        exit(1);

			continue;
		}

		ctrl_entry = malloc(sizeof(struct pci_vtdtr_ctrl_entry));
		assert(ctrl_entry != NULL);
		memset(ctrl_entry, 0, sizeof(struct pci_vtdtr_ctrl_entry));

		switch (ev.type) {
		case VTDTR_EV_INSTALL:
			ctrl = malloc(sizeof(struct pci_vtdtr_control));
			assert(ctrl != NULL);
			memset(ctrl, 0, sizeof(struct pci_vtdtr_control));

			ctrl->pvc_event = VTDTR_DEVICE_PROBE_INSTALL;
			ctrl->pvc_probeid = ev.args.p_toggle.probeid;
			break;

		case VTDTR_EV_UNINSTALL:
			ctrl = malloc(sizeof(struct pci_vtdtr_control));
			assert(ctrl != NULL);
			memset(ctrl, 0, sizeof(struct pci_vtdtr_control));

			ctrl->pvc_event = VTDTR_DEVICE_PROBE_UNINSTALL;
			ctrl->pvc_probeid = ev.args.p_toggle.probeid;
			break;

		case VTDTR_EV_GO:
			ctrl = malloc(sizeof(struct pci_vtdtr_control));
			assert(ctrl != NULL);
			memset(ctrl, 0, sizeof(struct pci_vtdtr_control));

			ctrl->pvc_event = VTDTR_DEVICE_GO;
			break;

		case VTDTR_EV_STOP:
			ctrl = malloc(sizeof(struct pci_vtdtr_control));
			assert(ctrl != NULL);
			memset(ctrl, 0, sizeof(struct pci_vtdtr_control));

			ctrl->pvc_event = VTDTR_DEVICE_STOP;
			error = dthyve_conf(1 << VTDTR_EV_RECONF, 0);
			assert(error == 0);
			break;

		case VTDTR_EV_ELF:
			elfpath = ev.args.elf_file.path;

			fd = dthyve_openelf(elfpath);
			if (fd == -1) {
				fprintf(stderr, "failed to open %s\n", elfpath);
				goto next;
			}

			st = vtdtr_get_filestat(fd);

			ctrl = vtdtr_elf_event(fd, offs, st);
			assert(ctrl != NULL);

			offs += ctrl->pvc_elflen;

			while (ctrl->pvc_elfhasmore == 1) {
				/*
				 * Enqueue the current control entry into the
				 * queue. We are always guaranteed to have one
				 * here due to the first allocation.
				 */
				ctrl_entry->ctrl = ctrl;

				pthread_mutex_lock(&sc->vsd_ctrlq->mtx);
				pci_vtdtr_cq_enqueue(sc->vsd_ctrlq, ctrl_entry);
				pthread_mutex_unlock(&sc->vsd_ctrlq->mtx);

				/*
				 * Get the new control element
				 */
				ctrl = vtdtr_elf_event(fd, offs, st);
				assert(ctrl != NULL);
				
				offs += ctrl->pvc_elflen;

				ctrl_entry = malloc(sizeof(struct pci_vtdtr_ctrl_entry));
				assert(ctrl_entry != NULL);
				memset(ctrl_entry, 0, sizeof(struct pci_vtdtr_ctrl_entry));
			}

			close(fd);
			break;

		case VTDTR_EV_RECONF:
			flags = 1 << VTDTR_EV_RECONF;
			char *vm = vm_get_name(sc->vsd_vmctx);

			if (pci_vtdtr_find(vm, ev.args.d_config.vms,
			    ev.args.d_config.count) == 0) {
				flags |= (1 << VTDTR_EV_INSTALL)	|
				    (1 << VTDTR_EV_STOP)		|
				    (1 << VTDTR_EV_ELF)			|
				    (1 << VTDTR_EV_GO);
			}

			error = dthyve_conf(flags, 0);
			assert(error == 0);
			/*
			 * We don't actually enqueue any event for this event,
			 * so we simply go to the start of the loop again.
			 *
			 * XXX: A bit ugly, but does the job.
			 */
			goto next;

		default:
			/*
			 * Hard fail if we catch something unexpected.
			 */
			errx(EXIT_FAILURE, "unexpected event %d", ev.type);
		}

		ctrl_entry->ctrl = ctrl;

		pthread_mutex_lock(&sc->vsd_ctrlq->mtx);
		pci_vtdtr_cq_enqueue(sc->vsd_ctrlq, ctrl_entry);
		pthread_mutex_unlock(&sc->vsd_ctrlq->mtx);

		pthread_mutex_lock(&sc->vsd_condmtx);
		pthread_cond_signal(&sc->vsd_cond);
		pthread_mutex_unlock(&sc->vsd_condmtx);
	}
}

/*
 * Mostly boilerplate, we initialize everything required for the correct
 * operation of the emulated PCI device, do error checking and finally dispatch
 * the communicator thread and add an event handler for kqueue.
 */
static int
pci_vtdtr_init(struct vmctx *ctx, struct pci_devinst *pci_inst, char *opts)
{
	struct pci_vtdtr_softc *sc;
	pthread_t communicator, reader;
	int error;

	error = 0;
	sc = calloc(1, sizeof(struct pci_vtdtr_softc));
	assert(sc != NULL);
	sc->vsd_ctrlq = calloc(1, sizeof(struct pci_vtdtr_ctrlq));
	assert(sc->vsd_ctrlq != NULL);
	STAILQ_INIT(&sc->vsd_ctrlq->head);

	vi_softc_linkup(&sc->vsd_vs, &vtdtr_vi_consts,
	    sc, pci_inst, sc->vsd_queues);
	sc->vsd_vs.vs_mtx = &sc->vsd_mtx;
	sc->vsd_vmctx = ctx;
	sc->vsd_ready = 0;

	sc->vsd_queues[0].vq_qsize = VTDTR_RINGSZ;
	sc->vsd_queues[0].vq_notify = pci_vtdtr_notify_tx;
	sc->vsd_queues[1].vq_qsize = VTDTR_RINGSZ;
	sc->vsd_queues[1].vq_notify = pci_vtdtr_notify_rx;

	pci_set_cfgdata16(pci_inst, PCIR_DEVICE, VIRTIO_DEV_DTRACE);
	pci_set_cfgdata16(pci_inst, PCIR_VENDOR, VIRTIO_VENDOR);
	pci_set_cfgdata8(pci_inst, PCIR_CLASS, PCIC_OTHER);
	pci_set_cfgdata16(pci_inst, PCIR_SUBDEV_0, VIRTIO_TYPE_DTRACE);
	pci_set_cfgdata16(pci_inst, PCIR_SUBVEND_0, VIRTIO_VENDOR);

	error = pthread_mutex_init(&sc->vsd_ctrlq->mtx, NULL);
	assert(error == 0);
	error = pthread_cond_init(&sc->vsd_cond, NULL);
	assert(error == 0);
	error = pthread_create(&communicator, NULL, pci_vtdtr_run, sc);
	assert(error == 0);
	if (dthyve_configured()) {
		error = pthread_create(&reader, NULL, pci_vtdtr_events, sc);
		assert(error == 0);
	}

	if (vi_intr_init(&sc->vsd_vs, 1, fbsdrun_virtio_msix()))
		return (1);

	vi_set_io_bar(&sc->vsd_vs, 0);
	return (0);
}

struct pci_devemu pci_de_vdtr = {
	.pe_emu		= "virtio-dtrace",
	.pe_init	= pci_vtdtr_init,
	.pe_barwrite	= vi_pci_write,
	.pe_barread	= vi_pci_read
};
PCI_EMUL_SET(pci_de_vdtr);
