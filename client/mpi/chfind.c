#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <fnmatch.h>
#include <getopt.h>
#include <mpi.h>
#include <chfs.h>

static struct option options[] = {
	{ "name", required_argument, NULL, 'n' },
	{ "size", required_argument, NULL, 's' },
	{ "newer", required_argument, NULL, 'N' },
	{ "type", required_argument, NULL, 't' },
	{ "version", no_argument, NULL, 'V' },
	{ 0, 0, 0, 0 }
};

static void
usage(void)
{
	fprintf(stderr, "usage: chfind [dir ...] [-name pat] [-size size] "
			"[-newer file]\n\t[-type type] [-version]\n");
	exit(EXIT_FAILURE);
}

static struct {
	char *name, type, *newer;
	size_t size;
	struct stat newer_sb;
} opt;

enum {
	FOUND,
	TOTAL,
	NUM_COUNT
};
static uint64_t local_count[NUM_COUNT], total_count[NUM_COUNT];

static int
find(const char *name, const struct stat *st)
{
	if (opt.newer && (st->st_mtim.tv_sec < opt.newer_sb.st_mtim.tv_sec ||
		(st->st_mtim.tv_sec == opt.newer_sb.st_mtim.tv_sec &&
		 st->st_mtim.tv_nsec < opt.newer_sb.st_mtim.tv_nsec))) {
		puts("old");
		return (0);
	}
	if (opt.size != -1 && st->st_size != opt.size) {
		puts("size");
		return (0);
	}
	if (opt.name && fnmatch(opt.name, name, 0)) {
		puts("name");
		return (0);
	}
	switch (opt.type) {
	case 'f':
		if (!S_ISREG(st->st_mode))
			return (0);
		break;
	case 'd':
		if (!S_ISDIR(st->st_mode))
			return (0);
		break;
	}
	local_count[FOUND]++;
	return (1);
}

struct dir {
	char *name;
	struct dir *next;
};

static struct dir *dir_head = NULL;
static struct dir **dir_tail = &dir_head;

static int
dir_list_push(char *name)
{
	struct dir *d = malloc(sizeof *d);

	if (d == NULL)
		return (-1);
	d->name = strdup(name);
	if (d->name == NULL) {
		free(d);
		return (-1);
	}
	d->next = NULL;
	*dir_tail = d;
	dir_tail = &d->next;
	return (0);
}

static char *
dir_list_pop(void)
{
	struct dir *d;
	char *name;

	if (dir_head == NULL)
		return (NULL);
	d = dir_head;
	dir_head = d->next;
	if (dir_head == NULL)
		dir_tail = &dir_head;
	name = d->name;
	free(d);
	return (name);
}

static int
filler(void *buf, const char *name, const struct stat *st, off_t off)
{
	char *d;

	local_count[TOTAL]++;

	if (name[0] == '.' && (name[1] == '\0' ||
		(name[1] == '.' && name[2] == '\0')))
			return (0);

	if (S_ISDIR(st->st_mode)) {
		d = malloc(strlen(buf) + 1 + strlen(name) + 1);
		sprintf(d, "%s/%s", (char *)buf, name);
		dir_list_push(d);
		free(d);
	}
	if (st->st_mode & CHFS_S_IFREP)
		return (0);
	if (find(name, st))
		printf("%s/%s\n", (char *)buf, name);
	return (0);
}

int
main(int argc, char *argv[])
{
	int c, rank, size;
	char *d;
	struct stat sb;

	MPI_Init(&argc, &argv);
	MPI_Comm_size(MPI_COMM_WORLD, &size);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);

	opt.size = -1;
	while ((c = getopt_long_only(argc, argv, "", options, NULL)) != -1) {
		switch (c) {
		case 'n':
			opt.name = optarg;
			break;
		case 'N':
			opt.newer = optarg;
			break;
		case 's':
			opt.size = atoi(optarg);
			break;
		case 't':
			opt.type = optarg[0];
			break;
		case 'V':
			fprintf(stderr, "CHFS version %s\n", chfs_version());
			exit(0);
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (opt.newer)
		if (lstat(opt.newer, &opt.newer_sb))
			perror(opt.newer), exit(EXIT_FAILURE);

	chfs_init(NULL);
	if (argc == 0) {
		if (chfs_stat(".", &sb)) {
			if (rank == 0)
				perror(".");
		} else {
			if (rank == 0) {
				if (find(".", &sb))
					printf(".\n");
				local_count[TOTAL]++;
			}
			dir_list_push(".");
		}
	} else
		for (; argc > 0; --argc) {
			if (chfs_stat(*argv, &sb)) {
				if (rank == 0)
					perror(*argv);
				continue;
			}
			if (rank == 0) {
				if (find(*argv, &sb))
					printf("%s\n", *argv);
				local_count[TOTAL]++;
			}
			dir_list_push(*argv++);
		}

	while ((d = dir_list_pop())) {
		if (size > 1)
			chfs_readdir_index(d, rank, d, filler);
		else
			chfs_readdir(d, d, filler);
		free(d);
	}
	MPI_Reduce(local_count, total_count, NUM_COUNT, MPI_LONG_LONG_INT,
		MPI_SUM, 0, MPI_COMM_WORLD);
	if (rank == 0)
		printf("MATHED %lu/%lu\n", total_count[FOUND],
			total_count[TOTAL]);
	chfs_term();
	MPI_Finalize();
	return (0);
}