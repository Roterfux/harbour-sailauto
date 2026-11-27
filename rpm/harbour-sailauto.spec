Name:       harbour-sailauto
Version:    0.14.3
Release:    1
Summary:    Android Auto projection to OpenAuto head units (MVP)
License:    GPLv3 and BSD
URL:        https://github.com/Roterfux/harbour-sailauto
Source0:    %{name}-%{version}.tar.bz2
Requires:   sailfishsilica-qt5 >= 0.10.9
Requires:   gstreamer1.0-plugins-base
Requires:   gstreamer1.0-plugins-bad
BuildRequires: pkgconfig(sailfishapp) >= 1.0.2
BuildRequires: pkgconfig(Qt5Core)
BuildRequires: pkgconfig(Qt5Qml)
BuildRequires: pkgconfig(Qt5Quick)
BuildRequires: pkgconfig(Qt5Network)
BuildRequires: pkgconfig(Qt5DBus)
Requires: qt5-plugin-sqldriver-sqlite
BuildRequires: pkgconfig(Qt5Svg)
BuildRequires: pkgconfig(Qt5Sql)
BuildRequires: pkgconfig(Qt5Positioning)
BuildRequires: pkgconfig(Qt5Contacts)
BuildRequires: pkgconfig(openssl)
BuildRequires: pkgconfig(gstreamer-1.0)
BuildRequires: pkgconfig(gstreamer-app-1.0)
BuildRequires: pkgconfig(libpulse)
BuildRequires: pkgconfig(dbus-1)
BuildRequires: desktop-file-utils

%description
Projects a car UI from Sailfish OS to an Android Auto head unit using the
Android Auto protocol over TCP (OpenAuto "wireless" mode). MVP: video and
touch, no audio yet. Not affiliated with Google.

%prep
%autosetup

%build
%qmake5 %{name}.pro
%make_build

%install
%qmake5_install
desktop-file-install --delete-original \
    --dir %{buildroot}%{_datadir}/applications \
    %{buildroot}%{_datadir}/applications/*.desktop
mv %{buildroot}/usr/lib/tmpfiles.d/harbour-sailauto.tmpfiles %{buildroot}/usr/lib/tmpfiles.d/harbour-sailauto.conf
# Contacts helper: same binary, setgid "privileged" (the contacts database is
# only readable for that group). Only this copy gets the group, the app does
# not (libdbus ignores the session bus address in setgid programs).
install -D -m 0755 %{buildroot}%{_bindir}/%{name} %{buildroot}/usr/libexec/%{name}/contacts-helper

%post
systemd-tmpfiles --create /usr/lib/tmpfiles.d/harbour-sailauto.conf || :
systemctl daemon-reload || :
systemctl enable harbour-sailauto-aoa.path || :
systemctl restart harbour-sailauto-aoa.path || :
# Autostart stays as the user set it; pick up a new script version.
systemctl try-restart harbour-sailauto-autostart.service || :

%preun
if [ $1 -eq 0 ]; then
    sh /usr/share/harbour-sailauto/aoa-gadget.sh off > /dev/null 2>&1 || :
    systemctl disable --now harbour-sailauto-aoa.path || :
    systemctl disable --now harbour-sailauto-autostart.service || :
fi

%postun
systemctl daemon-reload || :

%files
%{_bindir}/%{name}
%{_datadir}/%{name}/
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
%dir /usr/libexec/%{name}
%attr(2755, root, privileged) /usr/libexec/%{name}/contacts-helper
/usr/lib/systemd/system/harbour-sailauto-aoa.path
/usr/lib/systemd/system/harbour-sailauto-aoa.service
/usr/lib/systemd/system/harbour-sailauto-autostart.service
/usr/lib/systemd/user/harbour-sailauto-auto.service
/usr/lib/tmpfiles.d/harbour-sailauto.conf
