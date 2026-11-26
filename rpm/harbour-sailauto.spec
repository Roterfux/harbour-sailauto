Name:       harbour-sailauto
Version:    0.4.1
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
BuildRequires: pkgconfig(openssl)
BuildRequires: pkgconfig(gstreamer-1.0)
BuildRequires: pkgconfig(gstreamer-app-1.0)
BuildRequires: pkgconfig(libpulse)
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

%post
systemd-tmpfiles --create /usr/lib/tmpfiles.d/harbour-sailauto.conf || :
systemctl daemon-reload || :
systemctl enable harbour-sailauto-aoa.path || :
systemctl restart harbour-sailauto-aoa.path || :

%preun
if [ $1 -eq 0 ]; then
    sh /usr/share/harbour-sailauto/aoa-gadget.sh off > /dev/null 2>&1 || :
    systemctl disable --now harbour-sailauto-aoa.path || :
fi

%postun
systemctl daemon-reload || :

%files
%{_bindir}/%{name}
%{_datadir}/%{name}/
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
/usr/lib/systemd/system/harbour-sailauto-aoa.path
/usr/lib/systemd/system/harbour-sailauto-aoa.service
/usr/lib/tmpfiles.d/harbour-sailauto.conf
