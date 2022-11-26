#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <argp.h>
#include <libgen.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#define MAX_EVENTS 10

char *label_host = "host";
char *label_guest = "guest";

const char *argp_program_version = "0.0.1";
const char *argp_program_bug_address = "/dev/null";

static char doc[] = "evdevkm tool";

static char args_doc[] = "[Device...]";

static struct argp_option options[] = {
	{ "verbose", 'v', 0, 0, "Verbose output" },
	{ "grab", 'g', 0, 0, "Grab device" },
	{ "no-symlink", 'n', 0, 0, "Create no symlinks" },
	{ "user", 'u', 0, 0, "Uid or user name to assign to guest device" },
	{ 0 }
};

struct arguments {
	int verbose;
	int grab;
	int no_symlink;
	char *uid_or_user_name;
	struct Device *head;
};

enum TARGET {
	host,
	guest
};

char* target_label(enum TARGET target) {
	switch (target) {
		case host:
			return label_host;
		case guest:
			return label_guest;
	}
}			

enum TARGET flip_target(enum TARGET target) {
	switch (target) {
		case host:
			return guest;
		case guest:
			return host;
	}
}

struct DeviceTarget {
	struct libevdev_uinput *uidev;
	char *symlink_path;
};

struct Options {
	uid_t user_for_symbolic_link;
};

struct Device {
	char *device_path;

	int device_fd;
	struct libevdev *device;

	struct DeviceTarget *host;//TODO make non-pointer?
	struct DeviceTarget *guest;//TODO make non-pointer?

	struct Device *next;

	struct Options options;
};

struct DeviceTarget* device_target(struct Device *d, enum TARGET target) {
	switch (target) {
		case host:
			return d->host;
		case guest:
			return d->guest;
	}
}

int is_valid(struct Device *device) {
	struct stat st;

	if (stat(device->device_path, &st) < 0 && S_ISREG(st.st_mode) == false) {
		return false;
	}

	return true;
}

int is_readable(struct Device *device) {
	//TODO
	return 0;
}

int is_readable_and_writable(struct Device *device) {
	//TODO
	return 0;
}

int uid_from_string(uid_t *uid, char *uid_or_user_name) {
	//TODO
	return 0;
}

int initialize_options(struct Options **options) {
	//TODO
	return 0;
}

void free_device_target(struct DeviceTarget *t) {
	if (t->uidev != NULL) {
		libevdev_uinput_destroy(t->uidev);
		t->uidev = NULL;
	}

	if (t->symlink_path) {
		free(t->symlink_path);
		t->symlink_path = NULL;
	}

	free(t);
}

void free_device(struct Device *device) {
	if (device->device_path != NULL) {
		free(device->device_path);
		device->device_path = NULL;
	}

	if (device->device_fd != -1) {
		close(device->device_fd);
		device->device_fd = -1;
	}

	if (device->host != NULL) {
		free_device_target(device->host);
		device->host = NULL;
	}

	if (device->guest != NULL) {
		free_device_target(device->guest);
		device->guest = NULL;
	}

	free(device);
}

int epoll_add(int epfd, int fd, void *ptr) {
	struct epoll_event ev;

	ev.events = EPOLLIN;

	// note that data is an union and defaults to fd if ptr == NULL
	if (ptr != NULL) {
	  ev.data.ptr = ptr;
	} else {
		ev.data.fd = fd;
	}

  return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

int initialize_symlink_path(struct Device *d, enum TARGET target) {
	int rc = 0;
	char *path, *name, *label;
	size_t size;
	struct DeviceTarget *t;

	path = "/dev/input/by-path";
	name = basename(d->device_path);

	t = device_target(d, target);

	label = target_label(target);

	size = strlen(path)+strlen(name)+strlen(label)+4;

	t->symlink_path = malloc(sizeof(char)*size);
	if (t->symlink_path == NULL) {
		return -1;
	}

	rc = snprintf(t->symlink_path, size, "%s/%s-%s", path, name, label);
	if (rc < 0) {
		free(t->symlink_path);
		return rc;
	}

	return 0;
}

int file_exists(char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

int initialize_symlink(struct Device *d, enum TARGET target) {
	int rc;

	struct DeviceTarget *t = device_target(d,target);

	rc = initialize_symlink_path(d, target);
	if (rc < 0) { 
		fprintf(stderr, "failed to generate symlink path\n");
	}

	if (file_exists(t->symlink_path)); {
		rc = remove(t->symlink_path);
		if (rc < 0) {
			fprintf(stderr, "failed to remove %s with code %d\n", t->symlink_path, rc);
		}
	}

	rc = symlink(libevdev_uinput_get_devnode(t->uidev), t->symlink_path);
	if (rc < 0) {
		fprintf(stderr, "symlink creation failed for %s -> %s\n", 
			t->symlink_path, 
			libevdev_uinput_get_devnode(t->uidev));
		return rc;
	}

	return 0;
}

int initialize_drain(char *path) {
	int fd;
	char buffer[1024];
	ssize_t size;

	fd = open(path, O_RDONLY|O_NONBLOCK);

	do {
		size = read(fd, &buffer, 1024);
	} while (size > 0);

	close(fd);

	return 0;
}

int initialize_target(struct Device *device, enum TARGET target, int verbose) {
	int rc;
	char *label;
	struct DeviceTarget *t = device_target(device, target);

	label = target_label(target);

	rc = libevdev_uinput_create_from_device(device->device, LIBEVDEV_UINPUT_OPEN_MANAGED, &(t->uidev));
	if (rc < 0) {
		fprintf(stderr, "failed to create %s input\n", label);
		return rc;
	}

	if (verbose) {
		fprintf(stderr, "create uinput device: %s\n", libevdev_uinput_get_devnode(t->uidev));
	}

	rc = initialize_symlink(device, target);
	if (rc < 0) {
		return 0;
	}

	return 0;
}


int initialize(struct Device *device, int epfd, int grab, int verbose) {
	int rc, uifd;

	rc = initialize_drain(device->device_path);
	if (rc < 0) {
		fprintf(stderr, "failed to drain %s\n", device->device_path);
	}

	device->device_fd = open(device->device_path, O_RDONLY|O_NONBLOCK);
	if (device->device_fd < 0) {
		fprintf(stderr, "failed to on %s\n", device->device_path);
		return -1;
	}

	printf("d: %d\n", device->device_fd);

	rc = libevdev_new_from_fd(device->device_fd, &(device->device));
	if (rc < 0) {
		fprintf(stderr, "failed to initialize %s (%d)\n", device->device_path, rc);
		return rc;
	}

  rc = epoll_add(epfd, device->device_fd, device); 
	if (rc < 0) {
		fprintf(stderr, "failed to poll %s\n", device->device_path);
		return rc;
	}

	rc = initialize_target(device, host, verbose);
	if (rc < 0) {
		return 0;
	}

	rc = initialize_target(device, guest, verbose);
	if (rc < 0) {
		return 0;
	}

	if (grab) {
		rc = libevdev_grab(device->device, LIBEVDEV_GRAB);
		if (rc < 0) {
			fprintf(stderr, "failed to grab device");
			return rc;
		} 

		if (verbose) {
			fprintf(stderr, "grabbed device %s\n", device->device_path);
		}
	}

	return 0;
}

int next_event(struct Device *device, enum TARGET *target, int skip, int verbose) {
	int rc;
	enum TARGET previous_target;
	struct input_event ev;
	char *from, *to;

	rc = libevdev_next_event(device->device, LIBEVDEV_READ_FLAG_NORMAL, &ev);
	switch (rc) {
		case LIBEVDEV_READ_STATUS_SUCCESS:
			if (verbose) {
				printf("next event -> status success\n");
			}
			break;
		case LIBEVDEV_READ_STATUS_SYNC:
			if (verbose) {
				printf("next event -> status sync\n");
			}
			break;
		case -EAGAIN:
			break;
	}

	if (rc < 0) {
	return rc;
	}

	if (verbose) {
		printf("event: %s %s %d\n",
			libevdev_event_type_get_name(ev.type),
			libevdev_event_code_get_name(ev.type, ev.code),
			ev.value);
	}

	if (ev.type == EV_KEY && ev.code == KEY_RIGHTSHIFT && ev.value == 1) {
		previous_target = *target;

		*target = flip_target(*target);

		if (verbose) {
			from = target_label(previous_target);
			to = target_label(*target);

			printf("flipped target from %s to %s\n", from, to);
		}
	}

	if (skip == false) {
		switch (*target) {
			case host:
				rc = libevdev_uinput_write_event(device->host->uidev, ev.type, ev.code, ev.value);
				if (rc < 0) {
					fprintf(stderr, "failed write event\n");
					return rc;
				}
				break;
			case guest:
				rc = libevdev_uinput_write_event(device->guest->uidev, ev.type, ev.code, ev.value);
				if (rc < 0) {
					fprintf(stderr, "failed write event\n");
					return rc;
				}
				break;
		}
	}

	return 0;
}

int create_device_target(struct DeviceTarget **device_target, enum TARGET target) {
	struct DeviceTarget *t;

	t = malloc(sizeof(struct DeviceTarget));
	if (t == NULL) {
		return -1;
	}

	t->uidev = NULL;
	t->symlink_path = NULL;

	(*device_target) = t;

	return 0;
}

int create(struct Device **device, char *device_path) {
	int rc;
	struct Device *d;

	d = malloc(sizeof(struct Device));
	if (d == NULL) {
		return -1;
	}

	d->device_path = malloc(strlen(device_path)+1);
	if (d->device_path == NULL) {
		return -1;
	}

	strcpy(d->device_path, device_path);

	d->device_fd = -1;
	d->device = NULL;

	rc = create_device_target(&(d->host), host);
	if (rc < 0) {
		return rc;
	}

	rc = create_device_target(&(d->guest), guest);
	if (rc < 0) {
		return rc;
	}

	d->next = NULL;

	(*device) = d;

	return 0;
}

void append(struct Device **head, struct Device *device) {
	struct Device *d, *p;

	p = NULL;
	d = *head;

	while (d != NULL) {
		p = d;
		d = d->next;
	}

	d = device;

	if (p != NULL) {
		p->next = d;
	}

	if (*head == NULL) {
		*head = d;
	}
}


void free_all_devices(struct Device *head) {
	struct Device *d;

	if (head != NULL) {
		do {
			d = head;
			head = head->next;

			free(d);
			d = NULL;
		} while (head != NULL);
	}
}

int block_signals(int epfd) {
	int rc, signal_fd;
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	signal_fd = signalfd(-1, &mask, 0);
	if (signal_fd == -1) {
		fprintf(stderr, "failed to create signal file descriptor\n");
		return -1;
	}

	rc = sigprocmask(SIG_BLOCK, &mask, NULL);
	if (rc < 0) {
		fprintf(stderr, "failed to create sigprocmask\n");
		close(signal_fd);
		return rc;
	}

	rc = epoll_add(epfd, signal_fd, NULL);
	if (rc < 0) {
		fprintf(stderr, "failed to add signal file descriptor to epoll\n");
		close(signal_fd);
		return rc;
	}

	return signal_fd;
}

void cleanup(struct Device *head, int *epfd, int *signal_fd) {
	free_all_devices(head);

	if (!(*epfd < 0)) {
		close(*epfd);
		*epfd = -1;
	}

	if (!(*signal_fd < 0)) {
		close(*signal_fd);
		*signal_fd = -1;
	}
}

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
	int rc = 0;

	struct arguments *arguments = state->input;

	switch (key) {
		case 'v':
			arguments->verbose = true;
			break;
		case 'g':
			arguments->grab = true;
			break;
		case 'n':
			arguments->no_symlink = true;
			break;
		case 'u':
			arguments->uid_or_user_name = strdup(arg);
			if (arguments->uid_or_user_name == NULL) {
				fprintf(stderr, "failed to allocate memory for %s\n", arg);
				rc = ARGP_HELP_STD_ERR;
			}
			break;
		case ARGP_KEY_ARG:
			struct Device *d;

			rc = create(&d, arg);
			if (rc < 0) {
				rc = ARGP_HELP_STD_USAGE;
				break;
			}

			if (!is_valid(d)) {
				fprintf(stderr, "%s is not a valid device\n", arg);
				rc = ARGP_HELP_STD_ERR;
				break;
			}

			append(&(arguments->head), d);
			break;
	}

	if (rc < 0) {
		free_all_devices(arguments->head);

		if (arguments->uid_or_user_name == NULL) {
			free(arguments->uid_or_user_name);
			arguments->uid_or_user_name = NULL;
		}
	}

	return 0;
}

static struct argp argp = { options, parse_opt, args_doc, doc };

int main(int argc, char **argv) {
	int rc, epfd = -1, signal_fd = -1, nfds, n;
	struct arguments arguments;
	struct Device *d;
	struct epoll_event events[MAX_EVENTS];
	enum TARGET target = host;

	arguments.uid_or_user_name = NULL;
	arguments.head = NULL;

	argp_parse(&argp, argc, argv, 0, 0, &arguments);

	if (arguments.head != NULL) {
		epfd = epoll_create1(0);
		if (epfd < 0) {
			fprintf(stderr, "failed to create epoll file descriptor\n");
			cleanup(arguments.head,  &epfd, &signal_fd);
		};


		for (d = arguments.head; d != NULL; d = d->next) {
			if (!is_valid(d)) { 
				fprintf(stderr, "device %s is invalid\n", d->device_path);
				cleanup(arguments.head, &epfd, &signal_fd);
				exit(1);
			}

			if (initialize(d, epfd, arguments.grab, arguments.verbose) < 0) {
				fprintf(stderr, "device %s failed to initialize\n", d->device_path);
				cleanup(arguments.head,  &epfd, &signal_fd);
				exit(1);
			}
		}
		signal_fd = block_signals(epfd);
		if (signal_fd < 0) {
			fprintf(stderr, "failed to adapt interrupt signal to epoll\n");
			cleanup(arguments.head, &epfd, &signal_fd);
		}

		while (true) {
			nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);

			if (nfds == -1) {
				fprintf(stderr, "epoll failure\n");
				free_all_devices(arguments.head);
				exit(1);
			}

			for (n = 0; n < nfds; n++) {
				if (events[n].data.fd == signal_fd) {
					cleanup(arguments.head, &epfd, &signal_fd);
					exit(1);
				}

				if (events[n].data.ptr != NULL) {
					d = (struct Device *) events[n].data.ptr;

					for (rc = 0; !(rc < 0);) {
						rc = next_event(d, &target, false, arguments.verbose); 
						if (rc < 0) {
							fprintf(stderr, "failed next event processing with %d\n", rc);
						}
					}
				}
			}
		}

		cleanup(arguments.head, &epfd, &signal_fd);
	}
}
