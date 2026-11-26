TARGET = harbour-sailauto
QT += network quick qml dbus svg
CONFIG += c++14 link_pkgconfig

include(third_party/openh264/openh264.pri)

# App version comes from the RPM spec, so there is one place to change it.
APP_VERSION = $$system(sed -n \'s/^Version:[[:space:]]*//p\' $$PWD/rpm/harbour-sailauto.spec)
isEmpty(APP_VERSION): APP_VERSION = dev
DEFINES += APP_VERSION=\\\"$$APP_VERSION\\\"
PKGCONFIG += openssl gstreamer-1.0 gstreamer-app-1.0 libpulse dbus-1
# Caller name/photo from the address book (Sailfish: qtcontacts-sqlite)
packagesExist(Qt5Contacts) {
    QT += contacts
    DEFINES += HAVE_QTCONTACTS
}

desktop {
    # Headless test build:  qmake CONFIG+=desktop && make
    QT -= quick qml
    DEFINES += SAILAUTO_DESKTOP
} else {
    CONFIG += sailfishapp
    desktopfile.files = harbour-sailauto.desktop
    desktopfile.path = /usr/share/applications
    INSTALLS += desktopfile

    icon86.files = icons/86x86/harbour-sailauto.png
    icon86.path = /usr/share/icons/hicolor/86x86/apps
    icon108.files = icons/108x108/harbour-sailauto.png
    icon108.path = /usr/share/icons/hicolor/108x108/apps
    icon128.files = icons/128x128/harbour-sailauto.png
    icon128.path = /usr/share/icons/hicolor/128x128/apps
    icon172.files = icons/172x172/harbour-sailauto.png
    icon172.path = /usr/share/icons/hicolor/172x172/apps
    INSTALLS += icon86 icon108 icon128 icon172

    # Root helper for USB (Android Open Accessory). The app triggers it
    # through a systemd path unit; manual use: devel-su sh aoa-gadget.sh on
    aoascript.files = tools/aoa-gadget.sh tools/aoa-request.sh
    aoascript.path = /usr/share/harbour-sailauto
    aoaunits.files = system/harbour-sailauto-aoa.path system/harbour-sailauto-aoa.service \
        system/harbour-sailauto-autostart.service
    aoaunits.path = /usr/lib/systemd/system
    # Started by the autostart watcher inside the user's session.
    userunit.files = system/user/harbour-sailauto-auto.service
    userunit.path = /usr/lib/systemd/user
    aoatmp.files = system/harbour-sailauto.tmpfiles
    aoatmp.path = /usr/lib/tmpfiles.d
    INSTALLS += aoascript aoaunits aoatmp userunit
}

HEADERS += \
    src/aap/protobuf.h \
    src/aap/protocol.h \
    src/aap/cryptor.h \
    src/aap/session.h \
    src/aap/usbaccessory.h \
    src/aap/trace.h \
    src/video/carscreen.h \
    src/video/h264encoder.h \
    src/video/pcmh264.h \
    src/video/openh264enc.h \
    src/audio/audiotest.h \
    src/audio/audiorouter.h \
    src/media/mprisclient.h \
    src/phone/phonestatus.h \
    src/phone/notificationwatcher.h \
    src/phone/callmonitor.h \
    src/apps/appcatalog.h \
    src/nav/puremaps.h \
    src/controller.h

SOURCES += \
    src/main.cpp \
    src/aap/cryptor.cpp \
    src/aap/session.cpp \
    src/aap/usbaccessory.cpp \
    src/aap/trace.cpp \
    src/video/carscreen.cpp \
    src/video/h264encoder.cpp \
    src/video/pcmh264.cpp \
    src/video/openh264enc.cpp \
    src/audio/audiotest.cpp \
    src/audio/audiorouter.cpp \
    src/media/mprisclient.cpp \
    src/phone/phonestatus.cpp \
    src/phone/notificationwatcher.cpp \
    src/phone/callmonitor.cpp \
    src/apps/appcatalog.cpp \
    src/nav/puremaps.cpp \
    src/controller.cpp

DISTFILES += \
    harbour-sailauto.desktop \
    qml/harbour-sailauto.qml \
    qml/pages/MainPage.qml \
    qml/pages/LogPage.qml \
    qml/pages/AppsPage.qml \
    qml/cover/CoverPage.qml \
    rpm/harbour-sailauto.spec \
    tools/hu_sim.py \
    tools/aoa-gadget.sh \
    tools/aap_trace.py
