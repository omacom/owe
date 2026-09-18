#pragma once

#include <poll.h>
#include <stdbool.h>

#define OWE_FEED_MAX_CLIENTS 8
#define OWE_FEED_SLOTS 3

struct owe_feed;
struct owe_mpv;
struct owe_egl;

struct owe_feed *owe_feed_new(const char *socket_path, struct owe_egl *egl);
void owe_feed_free(struct owe_feed *f);

int owe_feed_fd(struct owe_feed *f);
void owe_feed_accept(struct owe_feed *f);
int owe_feed_pollfds(struct owe_feed *f, struct pollfd *fds);

/* Reads client frame acks and watches for disconnects. Pauses mpv while no
 * client is connected and resumes it when one arrives, so a lock without the
 * feed view costs nothing. */
void owe_feed_poll_clients(struct owe_feed *f, struct owe_mpv *m);

void owe_feed_start(struct owe_feed *f);
void owe_feed_stop(struct owe_feed *f);
bool owe_feed_running(struct owe_feed *f);

/* Publishes one decoded frame to every connected client. Returns 0 when a
 * frame was rendered and sent. */
int owe_feed_publish(struct owe_feed *f, struct owe_mpv *m);
