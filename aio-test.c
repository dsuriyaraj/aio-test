#define _GNU_SOURCE

#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/aio_abi.h>

#include "ccan/list/list.h"
#include "ccan/minmax/minmax.h"

// default values
#define OP_SIZE     512
#define NOPS        100000
#define WR_P        0
#define QUEUE_DEPTH 32

struct conf {
	size_t op_size;
	size_t nops;
	unsigned wr_p;
	size_t create_size;
	char *filename;
	size_t queue_depth;
	bool buffered;
};

struct io_op {
	char             *buff;
	struct iocb      iocb;
	struct list_node lnode;
};

struct io_op_slab {
	struct list_head io_ops;
	size_t total_nops;
};

static inline long
io_setup(unsigned maxevents, aio_context_t *ctx) {
    return syscall(SYS_io_setup, maxevents, ctx);
}

static inline long
io_submit(aio_context_t ctx, long nr, struct iocb **iocbpp) {
    return syscall(SYS_io_submit, ctx, nr, iocbpp);
}

static inline long
io_getevents(aio_context_t ctx, long min_nr, long nr,
                 struct io_event *events, struct timespec *timeout) {
    return syscall(SYS_io_getevents, ctx, min_nr, nr, events, timeout);
}

static inline int
io_destroy(aio_context_t ctx)
{
    return syscall(SYS_io_destroy, ctx);
}

static struct io_op *
alloc_io_op(size_t buff_size) {
	int err;
	struct io_op *ret = malloc(sizeof(*ret));
	if (!ret)
		return NULL;
	err = posix_memalign((void **)&ret->buff, 4096, buff_size);
	if (err) {
		free(ret);
		return NULL;
	}

	return ret;
}

static void
free_io_op(struct io_op *io) {
	free(io->buff);
	free(io);
}

static void
io_op_slab_init(struct io_op_slab *slab, size_t nops, size_t buff_size) {
	list_head_init(&slab->io_ops);
	for (size_t i=0; i<nops; i++) {
		struct io_op *op = alloc_io_op(buff_size);
		if (!op) {
			fprintf(stderr, "alloc_io_op failed\n");
			abort();
		}
		list_add_tail(&slab->io_ops, &op->lnode);
	}
	slab->total_nops = nops;
}

static void
io_op_slab_destroy(struct io_op_slab *slab) {
	size_t nops = 0;
	struct io_op *op;
	while ((op = list_pop(&slab->io_ops, struct io_op, lnode))) {
		free_io_op(op);
		nops++;
	}

	if (nops != slab->total_nops)
		fprintf(stderr, "Error: leaked io ops (freed:%zd, total:%zd)\n", nops, slab->total_nops);

	slab->total_nops = 0;
}


static struct io_op *
get_io_op(struct io_op_slab *slab) {
	return list_pop(&slab->io_ops, struct io_op, lnode);
}

static void
put_io_op(struct io_op_slab *slab, struct io_op *op) {
	list_add(&slab->io_ops, &op->lnode);
}

static inline void
io_op_fill_iocb(struct io_op *op, int fd, size_t total_size, struct conf *cnf) {

	size_t nblocks = total_size / cnf->op_size;
	size_t block = rand() % nblocks;
	size_t offset = block*cnf->op_size;
	unsigned wr_p = 1 + (rand() % 100);

	memset(&op->iocb, 0, sizeof(op->iocb));
	op->iocb.aio_buf        = (uintptr_t)op->buff;
	op->iocb.aio_data       = (uintptr_t)op;
	op->iocb.aio_fildes     = fd;
	op->iocb.aio_lio_opcode = (wr_p <= cnf->wr_p) ? IOCB_CMD_PWRITE : IOCB_CMD_PREAD;
	op->iocb.aio_reqprio    = 0;
	op->iocb.aio_nbytes     = cnf->op_size;
	op->iocb.aio_offset     = offset;
}

static void
print_help(FILE *f, char *p) {
	fprintf(f,"Usage: %s [-o op_size] [-n nops] [-w write_percentage] [-c creat_file_size] [-q queue_depth] [-b (for buffered IO)] -f filename\n", p);
}

static void
parse_conf(struct conf *cnf, int argc, char *argv[]) {

	int opt;
	while ((opt = getopt(argc, argv, "ho:n:w:f:c:q:b")) != -1) {
		switch (opt) {
			case 'o':
			cnf->op_size = atol(optarg);
			break;

			case 'n':
			cnf->nops = atol(optarg);
			break;

			case 'w':
			cnf->wr_p = atol(optarg);
			break;

			case 'f':
			cnf->filename = optarg;
			break;

			case 'c':
			cnf->create_size = atol(optarg);
			break;

			case 'q':
			cnf->queue_depth = atol(optarg);
			break;

			case 'b':
			cnf->buffered = true;
			break;

			case 'h':
			print_help(stdout, argv[0]);
			exit(0);

			default:
			print_help(stderr, argv[0]);
			exit(1);
		}
	}
}
static int
open_file(struct conf *cnf) {

	int fd, oflags;
	if (cnf->wr_p == 0) {
		oflags = O_RDONLY;
	} else if (cnf->wr_p == 100) {
		oflags = O_WRONLY;
	} else if (cnf->wr_p < 100) {
		oflags = O_RDWR;
	} else {
		fprintf(stderr, "percentage %u is >100\n", cnf->wr_p);
		exit(1);
	}

	if (!cnf->buffered)
		oflags |= O_DIRECT;

	if (cnf->create_size > 0) {
		oflags |= O_CREAT;
	}

	if ((fd = open(cnf->filename, oflags, S_IRUSR | S_IWUSR)) == -1) {
		perror(cnf->filename);
		exit(1);
	}

	if (cnf->create_size > 0) {
		if (truncate(cnf->filename, cnf->create_size) == -1) {
			perror("truncate");
			exit(1);
		}
	}

	return fd;
}

int main(int argc, char *argv[])
{
	struct conf cnf = {
		.filename = NULL,
		.op_size = OP_SIZE,
		.nops = NOPS,
		.wr_p = WR_P,
		.create_size = 0,
		.queue_depth = QUEUE_DEPTH,
		.buffered = false,
	};

	parse_conf(&cnf, argc, argv);

	if (!cnf.filename) {
		print_help(stderr, argv[0]);
		exit(1);
	}


	printf("CONF: filename:%s op_size:%zd nops:%zd wr_p:%u create_size:%zd queue_depth:%zd buffered:%u\n",
	        cnf.filename, cnf.op_size, cnf.nops, cnf.wr_p, cnf.create_size, cnf.queue_depth, cnf.buffered);


	int fd;
	struct stat st;
	aio_context_t ioctx = 0;
	struct io_op_slab slab;

	fd = open_file(&cnf);

	if (fstat(fd, &st) == -1) {
		perror("fstat");
		exit(1);
	}
	assert(st.st_size >= cnf.op_size);

	if (io_setup(cnf.queue_depth, &ioctx) < 0) {
		perror("io_setup");
		exit(1);
	}

	io_op_slab_init(&slab, cnf.queue_depth, cnf.op_size);

	struct iocb *iocb_ptrs[cnf.queue_depth];
	struct io_event io_events[cnf.queue_depth];
	size_t submitted = 0, completed = 0;
	while (completed < cnf.nops) {
		long ret;
		assert(submitted >= completed);
		size_t in_flight = submitted - completed;
		assert(in_flight <= cnf.queue_depth);
		size_t to_submit = min(cnf.queue_depth - in_flight, cnf.nops - submitted);

		for (size_t i=0; i<to_submit; i++) {
			struct io_op *op = get_io_op(&slab);
			assert(op);
			io_op_fill_iocb(op, fd, st.st_size, &cnf);
			iocb_ptrs[i] = &op->iocb;
		}

		ret = io_submit(ioctx, to_submit, iocb_ptrs);
		if (ret < 0) {
			perror("io_submit");
			exit(1);
		} else if (ret != to_submit) {
			fprintf(stderr, "Partial success (%zd instead of %zd). Bailing out\n", ret, to_submit);
			exit(1);
		}
		submitted += to_submit;

		ret = io_getevents(ioctx, 0 /* min */, submitted - completed, io_events, NULL);
		if (ret < 0) {
			perror("io_getevents");
			exit(1);
		}

		size_t to_complete = ret;
		for (size_t i=0; i<to_complete; i++) {
			struct io_event *ev = &io_events[i];
			if (ev->res2 != 0 || ev->res != cnf.op_size)
				fprintf(stderr, "******************** Event returned with res=%lld res2=%lld\n", ev->res, ev->res2);
			struct io_op *op = (void *)ev->data;
			put_io_op(&slab, op);
		}
		completed += to_complete;

		printf("in_flight:%zd submitted:%zd (total:%zd) completed:%zd (total:%zd)\n", submitted - completed, to_submit, submitted, to_complete, completed);
	}

	io_op_slab_destroy(&slab);
	io_destroy(ioctx);
	close(fd);
	return 0;

}
