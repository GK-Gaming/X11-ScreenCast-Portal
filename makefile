drmtap_headers = -Ilibdrmtap/include
pipewire_headers = -I/usr/include/spa-0.2 -I/usr/include/pipewire-0.3

portal_libs = -ldrmtap -lsystemd
session_libs = -lpipewire-0.3 -ldrmtap -lm -lsystemd -lEGL -lGL

cflags = -ggdb

all: xdg-desktop-portal-x11 xdg-desktop-portal-x11-session

xdg-desktop-portal-x11: portal.c
	$(CC) -o xdg-desktop-portal-x11 portal.c $(drmtap_headers) $(pipewire_headers) $(portal_libs) $(cflags)

xdg-desktop-portal-x11-session: portal-session.c
	$(CC) -o xdg-desktop-portal-x11-session portal-session.c $(drmtap_headers) $(pipewire_headers) $(session_libs) $(cflags)

session-caps:
	sudo setcap cap_sys_admin+ep xdg-desktop-portal-x11-session

run: all session-caps
	./xdg-desktop-portal-x11

install: all session-caps
	sudo cp portal-sys-files/x11-portals.conf /usr/share/xdg-desktop-portal/
	sudo cp x11.portal /usr/share/xdg-desktop-portal/portals/
	sudo cp org.freedesktop.impl.portal.desktop.x11.service-files/x11-portals.conf /usr/share/dbus-1/services/
	sudo cp xdg-desktop-portal-x11.service /usr/lib/systemd/user/
	sudo cp xdg-desktop-portal-x11 /lib/
	sudo cp xdg-desktop-portal-x11-session /lib/
	systemctl --user restart xdg-desktop-portal

