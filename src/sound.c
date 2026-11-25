/*
 *  Window Maker window manager
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
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "wconfig.h"

#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef USE_CANBERRA
#include <canberra.h>
#endif

#include <WINGs/WUtil.h>

#include "WindowMaker.h"
#include "window.h"
#include "event.h"
#include "osdep.h"
#include "sound.h"


static Bool initialized = False;

Bool wSoundEventEnabled(const char *event_id)
{
	char **e;

	if (!wPreferences.sound_events)
		return False;

	for (e = wPreferences.sound_events; *e; e++)
		if (strcmp(*e, event_id) == 0)
			return True;

	return False;
}

#ifdef USE_CANBERRA

/*
 * Each sound is played by a short-lived child process: talking to the sound
 * server can block (a server that accepts the connection and doesn't answer
 * keeps libcanberra waiting), and Window Maker must never wait for it. The
 * child doesn't use the connection to the X server, and it is killed if the
 * sound takes too long.
 */
#define SOUND_TIMEOUT		10	/* s */
#define SOUND_MAX_CHILDREN	4

static int sound_children = 0;

static void sound_child_died(pid_t pid, unsigned int status, void *cdata)
{
	static Bool warned = False;

	(void) pid;
	(void) cdata;

	if (sound_children > 0)
		sound_children--;

	/* the child exits with the error of libcanberra, as a positive number */
	if (status != 0 && !warned) {
		wwarning(_("could not play a sound: %s"), ca_strerror(-(int) status));
		warned = True;
	}
}

static void play_in_child(const char *event_id, const char *description, const char *theme)
{
	static const struct timespec tick = { 0, 50 * 1000000L };
	ca_context *context;
	sigset_t sigs;
	long fd;
	int ret, playing;

	/*
	 * Nothing of Window Maker is used here: close its descriptors, among
	 * them the connection to the X server, so that the child doesn't keep
	 * them open after Window Maker exits or restarts
	 */
	for (fd = sysconf(_SC_OPEN_MAX) - 1; fd > 2; fd--)
		close(fd);

	/* don't keep the handlers and the mask of Window Maker */
	sigemptyset(&sigs);
	sigprocmask(SIG_SETMASK, &sigs, NULL);
	signal(SIGALRM, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
	alarm(SOUND_TIMEOUT);

	ret = ca_context_create(&context);
	if (ret != CA_SUCCESS)
		_exit(-ret);

	ca_context_change_props(context,
	                        CA_PROP_APPLICATION_NAME, "Window Maker",
	                        CA_PROP_APPLICATION_ID, "org.windowmaker.WindowMaker",
	                        NULL);

	ret = ca_context_play(context, 1,
	                      CA_PROP_EVENT_ID, event_id,
	                      CA_PROP_EVENT_DESCRIPTION, description,
	                      CA_PROP_CANBERRA_XDG_THEME_NAME, theme,
	                      NULL);

	/* the sound stops when the context is destroyed: wait for its end */
	if (ret == CA_SUCCESS) {
		playing = 1;
		while (ca_context_playing(context, 1, &playing) == CA_SUCCESS && playing)
			nanosleep(&tick, NULL);
	}

	ca_context_destroy(context);

	/* a theme without this sound, or sounds disabled, are not errors */
	if (ret == CA_ERROR_NOTFOUND || ret == CA_ERROR_DISABLED)
		ret = CA_SUCCESS;
	_exit(-ret);
}

void wSoundPlay(const char *event_id, const char *description)
{
	static Bool warned = False;
	const char *theme;
	pid_t pid;

	if (!initialized || !wSoundEventEnabled(event_id))
		return;

	/* sounds that don't end (a stalled server) don't pile up */
	if (sound_children >= SOUND_MAX_CHILDREN)
		return;

	theme = wPreferences.sound_theme;
	if (theme == NULL || theme[0] == '\0')
		theme = "freedesktop";

	pid = fork();
	if (pid < 0) {
		if (!warned) {
			werror(_("could not create a process to play a sound"));
			warned = True;
		}
		return;
	}
	if (pid == 0)
		play_in_child(event_id, description, theme);

	sound_children++;
	wAddDeathHandler(pid, sound_child_died, NULL);
}

/*
 * Charger plugged in or out: the kernel's power_supply uevents are waited
 * for together with the X events, only while power-plug or power-unplug is
 * in SoundEvents. The charger can come online a moment after the event of
 * its connector, so after an event that changed nothing the supply is read
 * again a few times, then not until the next event.
 */
#define POWER_RECHECK_DELAY	500	/* ms */
#define POWER_RECHECKS		10
#define POWER_QUIET		3000	/* ms without a second sound for the same change */

static int power_fd = -1;
static WMHandlerID power_handler = NULL;
static WMHandlerID power_timer = NULL;
static int power_rechecks_left;
static int mains_online = -1;

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Reads the supply; returns True if it changed (and the sound was played) */
static Bool power_check(void)
{
	static long last_sound[2] = { -POWER_QUIET, -POWER_QUIET };
	int online, previous;
	long now;

	online = wOSMainsOnline();
	if (online < 0)
		return False;

	previous = mains_online;
	mains_online = online;
	if (previous < 0 || previous == online)
		return False;

	now = now_ms();
	if (now - last_sound[online] >= POWER_QUIET) {
		last_sound[online] = now;
		if (online)
			wSoundPlay("power-plug", "Power cable plugged in");
		else
			wSoundPlay("power-unplug", "Power cable unplugged");
	}
	return True;
}

static void power_recheck(void *cdata)
{
	(void) cdata;

	power_timer = NULL;
	if (power_check() || --power_rechecks_left <= 0)
		return;

	power_timer = WMAddTimerHandler(POWER_RECHECK_DELAY, power_recheck, NULL);
}

static void power_monitor_stop(void);

static void handle_power_events(int fd, int mask, void *cdata)
{
	int ret;

	(void) mask;
	(void) cdata;

	ret = wOSReadPowerEvents(fd);
	if (ret < 0) {
		wwarning(_("could not read the power supply events, the charger sounds are off"));
		power_monitor_stop();
		return;
	}
	if (ret == 0)
		return;

	if (power_check()) {
		if (power_timer) {
			WMDeleteTimerHandler(power_timer);
			power_timer = NULL;
		}
		return;
	}

	/* a burst of events doesn't make the rechecks last longer */
	if (power_timer == NULL) {
		power_rechecks_left = POWER_RECHECKS;
		power_timer = WMAddTimerHandler(POWER_RECHECK_DELAY, power_recheck, NULL);
	}
}

static void power_monitor_start(void)
{
	if (power_fd >= 0)
		return;

	power_fd = wOSOpenPowerEvents();
	if (power_fd < 0)
		return;

	mains_online = wOSMainsOnline();
	power_handler = WMAddInputHandler(power_fd, WIReadMask, handle_power_events, NULL);
}

static void power_monitor_stop(void)
{
	if (power_timer) {
		WMDeleteTimerHandler(power_timer);
		power_timer = NULL;
	}
	if (power_handler) {
		WMDeleteInputHandler(power_handler);
		power_handler = NULL;
	}
	if (power_fd >= 0) {
		close(power_fd);
		power_fd = -1;
	}
	mains_online = -1;
}

void wSoundEventsChanged(void)
{
	if (!initialized)
		return;

	if (wSoundEventEnabled("power-plug") || wSoundEventEnabled("power-unplug"))
		power_monitor_start();
	else
		power_monitor_stop();
}

#else /* !USE_CANBERRA */

void wSoundPlay(const char *event_id, const char *description)
{
	(void) event_id;
	(void) description;
}

void wSoundEventsChanged(void)
{
}

#endif /* USE_CANBERRA */

/* Called once the defaults are read, in the process that manages the screens */
void wSoundInitialize(void)
{
	initialized = True;
	wSoundEventsChanged();
}
