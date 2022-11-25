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

//TODO refactor methods to reduce code

const char *argp_program_version = "0.0.1";
const char *argp_program_bug_address = "/dev/null";

static char doc[] = "evdevkm tool";

static char args_doc[] = "";

static struct argp_option options[] = {
	{ "verbose", 'v', 0, 0, "Verbose output" },
	{ "grab", 'g', 0, 0, "Grab device" },
	{ "no-symlink", 'n', 0, 0, "Create no symlinks" },
	{ 0 }
};

struct arguments {
	int verbose;
	int grab;
	int no_symlink;
	struct Device *head;
};

enum TARGET {
	host,
	guest
};

void target_name(enum TARGET target, char *name, int name_length) {
	switch (target) {
		case host:
			strncpy(name, "host", name_length);
			break;
		case guest:
			strncpy(name, "guest", name_length);
			break;
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

struct Device {
	char *device_path;

	int device_fd;
	struct libevdev *device;

	struct libevdev_uinput *uidev_host;
	char *uidev_host_symlink_path;

	struct libevdev_uinput *uidev_guest;
	char *uidev_guest_symlink_path;

	struct Device *next;
};

int is_valid(struct Device *device) {
	struct stat st;

	if (stat(device->device_path, &st) < 0 && S_ISREG(st.st_mode) == false) {
	return false;
	}

	return true;
}

int is_readable(struct Device *device) {
	return 0;//TODO
}

int is_readable_and_writable(struct Device *device) {
	return 0;//TODO
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

	if (device->uidev_host != NULL) {
		libevdev_uinput_destroy(device->uidev_host);
		device->uidev_host = NULL;
	}

	if (device->uidev_host_symlink_path != NULL) {
		free(device->uidev_host_symlink_path);
		device->uidev_host_symlink_path = NULL;
	}

	if (device->uidev_guest != NULL) {
		libevdev_uinput_destroy(device->uidev_guest);
		device->uidev_guest = NULL;
	}

	if (device->uidev_guest_symlink_path != NULL) {
		free(device->uidev_guest_symlink_path);
		device->uidev_guest_symlink_path = NULL;
	}
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

int initialize_symlink_paths(struct Device *device) {
	int rc;
	char *path, *copy, *name;
	size_t size;

	path = "/dev/input/by-path";

	copy = strdup(device->device_path);
	name = basename(copy);

	size = strlen(path)+strlen(name)+7;
	device->uidev_host_symlink_path = malloc(sizeof(char)*size);
	rc = snprintf(device->uidev_host_symlink_path, size, "%s/%s-%s", path, name, "host");

	if (rc < 0) {
		free(copy);
		return rc;
	}

	size = strlen(path)+strlen(name)+8;
	device->uidev_guest_symlink_path = malloc(sizeof(char)*size);
	rc = snprintf(device->uidev_guest_symlink_path, size, "%s/%s-%s", path, name, "guest");

	if (rc < 0) {
		free(copy);
		return rc;
	}

	free(copy);
	return rc;
}

int file_exists(char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

int initialize_symlinks(struct Device *device) {
	int rc;

	if (file_exists(device->uidev_host_symlink_path)) {
		rc = remove(device->uidev_host_symlink_path);
		if (rc < 0) {
			printf("failed to remove %s with code %d\n", device->uidev_host_symlink_path, rc);
		}
	}

	rc = symlink(libevdev_uinput_get_devnode(device->uidev_host), device->uidev_host_symlink_path);
	if (rc < 0) {
		printf("symlink creation failed for %s -> %s\n", 
			device->uidev_host_symlink_path, 
			libevdev_uinput_get_devnode(device->uidev_host));
		return rc;
	}

	if (file_exists(device->uidev_guest_symlink_path)) {
		rc = remove(device->uidev_guest_symlink_path);
	}

	rc = symlink(libevdev_uinput_get_devnode(device->uidev_guest), device->uidev_guest_symlink_path);
	if (rc < 0) {
		printf("symlink creation failed for %s -> %s\n", 
			device->uidev_guest_symlink_path, 
			libevdev_uinput_get_devnode(device->uidev_guest));
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

int initialize(struct Device *device, int epfd, int grab, int verbose) {
	int rc, uifd;

	rc = initialize_drain(device->device_path);
	if (rc < 0) {
		printf("failed to drain %s\n", device->device_path);
	}

	device->device_fd = open(device->device_path, O_RDONLY|O_NONBLOCK);
	if (device->device_fd < 0) {
		printf("failed to on %s\n", device->device_path);
		return -1;
	}
	printf("d: %d\n", device->device_fd);

	rc = libevdev_new_from_fd(device->device_fd, &(device->device));
	if (rc < 0) {
		printf("failed to initialize %s (%d)\n", device->device_path, rc);
		return rc;
	}

  rc = epoll_add(epfd, device->device_fd, device); 
	if (rc < 0) {
		printf("failed to poll %s\n", device->device_path);
		return rc;
	}

	rc = libevdev_uinput_create_from_device(device->device, LIBEVDEV_UINPUT_OPEN_MANAGED, &(device->uidev_host));
	if (rc < 0) {
		printf("failed to create host uinput\n");
		return rc;
	}

	if (verbose) {
		printf("Host uinput device: %s\n", libevdev_uinput_get_devnode(device->uidev_host));
	}

	rc = libevdev_uinput_create_from_device(device->device, LIBEVDEV_UINPUT_OPEN_MANAGED, &(device->uidev_guest));
	if (rc < 0) {
		printf("failed to create guest uinput\n");
		return rc;
	}

	if (verbose) {
		printf("Guest uinput device: %s\n", libevdev_uinput_get_devnode(device->uidev_guest));
	}
	
	rc = initialize_symlink_paths(device);
	if (rc < 0) {
		return rc;
	}

	if (verbose) {
		printf("symlink path host:  %s\n", device->uidev_host_symlink_path);
		printf("symlink path guest: %s\n", device->uidev_guest_symlink_path);
	}

	rc = initialize_symlinks(device);
	if (rc < 0) {
		printf("failed to create symlinks");
		return rc;
	}

	if (grab) {
		rc = libevdev_grab(device->device, LIBEVDEV_GRAB);
		if (rc < 0) {
			printf("failed to grab device");
			return rc;
		} 

		if (verbose) {
			printf("grabbed device %s\n", device->device_path);
		}
	}

	return 0;
}

int next_event(struct Device *device, enum TARGET *target, int skip, int verbose) {
	int rc;
	struct input_event ev;

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
		enum TARGET previous_target = *target;

		*target = flip_target(*target);

		if (verbose) {
			char from[6], to[6];

			target_name(previous_target, from, 6);
			target_name(*target, to, 6);

			printf("flipped target from %s to %s\n", from, to);
		}
	}

	if (skip == false) {
		switch (*target) {
			case host:
				rc = libevdev_uinput_write_event(device->uidev_host, ev.type, ev.code, ev.value);
				if (rc < 0) {
					printf("failed write event\n");
					return rc;
				}
				break;
			case guest:
				rc = libevdev_uinput_write_event(device->uidev_guest, ev.type, ev.code, ev.value);
				if (rc < 0) {
					printf("failed write event\n");
					return rc;
				}
				break;
		}
	}

	return 0;
}

int create(struct Device **device, char *device_path) {
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
	d->uidev_host = NULL;
	d->uidev_host_symlink_path = NULL;
	d->uidev_guest = NULL;
	d->uidev_guest_symlink_path = NULL;
	d->next = NULL;

	(*device) = d;

	return true;
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
		fprintf(stderr, "failed to create signal file descriptor");
		return -1;
	}

	rc = sigprocmask(SIG_BLOCK, &mask, NULL);
	if (rc < 0) {
		fprintf(stderr, "failed to create sigprocmask");
		close(signal_fd);
		return rc;
	}

	rc = epoll_add(epfd, signal_fd, NULL);
	if (rc < 0) {
		fprintf(stderr, "failed to add signal file descriptor to epoll");
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
	int rc;

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
		case ARGP_KEY_ARG:
			struct Device *d;

			rc = create(&d, arg);
			if (rc == false) {
				return ARGP_HELP_STD_USAGE;
			}

			if (!is_valid(d)) {
				printf("%s is not a valid device", arg);
				return ARGP_HELP_STD_ERR;
			}

			append(&(arguments->head), d);

			break;
	}

	return 0;
};

static struct argp argp = { options, parse_opt, args_doc, doc };

int main(int argc, char **argv) {
	int rc, epfd = -1, signal_fd = -1, nfds, n;
	struct arguments arguments;
	struct Device *d;
	struct epoll_event events[MAX_EVENTS];
	enum TARGET target = host;

	arguments.head = NULL;

	argp_parse(&argp, argc, argv, 0, 0, &arguments);

	if (arguments.head != NULL) {
		epfd = epoll_create1(0);
		if (epfd < 0) {
			fprintf(stderr, "failed to create epoll file descriptor");
			cleanup(arguments.head,  &epfd, &signal_fd);
		};


		for (d = arguments.head; d != NULL; d = d->next) {
			if (!is_valid(d)) { 
				printf("device %s is invalid\n", d->device_path);
				cleanup(arguments.head, &epfd, &signal_fd);
				exit(1);
			}

			if (initialize(d, epfd, arguments.grab, arguments.verbose) < 0) {
				printf("device %s failed to initialize\n", d->device_path);
				cleanup(arguments.head,  &epfd, &signal_fd);
				exit(1);
			}
		}
		signal_fd = block_signals(epfd);
		if (signal_fd < 0) {
			fprintf(stderr, "failed to adapt interrupt signal to epoll");
			cleanup(arguments.head, &epfd, &signal_fd);
		}

		while (true) {
			nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);

			if (nfds == -1) {
				fprintf(stderr, "epoll failure");
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
