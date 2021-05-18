#-------------------------------------------------
#
# Project created by QtCreator 2016-08-12T21:55:19
#
#-------------------------------------------------

# Version
VT_VERSION = 45.00
VT_INTRO_VERSION = 1
VT_IS_TEST_VERSION = 1

VT_ANDROID_VERSION_ARMV7 = 95
VT_ANDROID_VERSION_ARM64 = 96
VT_ANDROID_VERSION_X86 = 97

VT_ANDROID_VERSION = $$VT_ANDROID_VERSION_X86

# Ubuntu 18.04 (should work on raspbian buster too)
# sudo apt install qml-module-qt-labs-folderlistmodel qml-module-qtquick-extras qml-module-qtquick-controls2 qt5-default libqt5quickcontrols2-5 qtquickcontrols2-5-dev qtcreator qtcreator-doc libqt5serialport5-dev build-essential qml-module-qt3d qt3d5-dev qtdeclarative5-dev qtconnectivity5-dev qtmultimedia5-dev qtpositioning5-dev qtpositioning5-dev libqt5gamepad5-dev qml-module-qt-labs-settings

DEFINES += VT_VERSION=$$VT_VERSION
DEFINES += VT_INTRO_VERSION=$$VT_INTRO_VERSION
!vt_test_version: {
    DEFINES += VT_IS_TEST_VERSION=$$VT_IS_TEST_VERSION
}
vt_test_version: {
    DEFINES += VT_IS_TEST_VERSION=1
}

CONFIG += c++11
QMAKE_CXXFLAGS += -Wno-deprecated-copy

# Build mobile GUI
#CONFIG += build_mobile

# Debug build (e.g. F5 to reload QML files)
#DEFINES += DEBUG_BUILD

# If BLE disconnects on ubuntu after about 90 seconds the reason is most likely that the connection interval is incompatible. This can be fixed with:
# sudo bash -c 'echo 6 > /sys/kernel/debug/bluetooth/hci0/conn_min_interval'

# Clear old bluetooth devices
# sudo rm -rf /var/lib/bluetooth/*
# sudo service bluetooth restart

# Bluetooth available
DEFINES += HAS_BLUETOOTH

# CAN bus available
# Adding serialbus to Qt seems to break the serial port on static builds. TODO: Figure out why.
#DEFINES += HAS_CANBUS

# Positioning
DEFINES += HAS_POS

!android: {
    # Serial port available
    DEFINES += HAS_SERIALPORT
    DEFINES += HAS_GAMEPAD
}
win32: {
    DEFINES += _USE_MATH_DEFINES
}

# Options
#CONFIG += build_vari

QT       += core gui
QT       += widgets
QT       += printsupport
QT       += network
QT       += quick
QT       += quickcontrols2

contains(DEFINES, HAS_SERIALPORT) {
    QT       += serialport
}

contains(DEFINES, HAS_CANBUS) {
    QT       += serialbus
}

contains(DEFINES, HAS_BLUETOOTH) {
    QT       += bluetooth
}

contains(DEFINES, HAS_POS) {
    QT       += positioning
}

contains(DEFINES, HAS_GAMEPAD) {
    QT       += gamepad
}

android: QT += androidextras

android: TARGET = esc_tool
!android: TARGET = esc_tool_$$VT_VERSION

ANDROID_VERSION = 1

android:contains(QT_ARCH, i386) {
    VT_ANDROID_VERSION = $$VT_ANDROID_VERSION_X86
}

contains(ANDROID_TARGET_ARCH, arm64-v8a) {
    VT_ANDROID_VERSION = $$VT_ANDROID_VERSION_ARM64
}

contains(ANDROID_TARGET_ARCH, armeabi-v7a) {
    VT_ANDROID_VERSION = $$VT_ANDROID_VERSION_ARMV7
}

android: {
    manifest.input = $$PWD/android/AndroidManifest.xml.in
    manifest.output = $$PWD/android/AndroidManifest.xml
    QMAKE_SUBSTITUTES += manifest
}

TEMPLATE = app

release_win {
    DESTDIR = build/win
    OBJECTS_DIR = build/win/obj
    MOC_DIR = build/win/obj
    RCC_DIR = build/win/obj
    UI_DIR = build/win/obj
}

release_lin {
    # http://micro.nicholaswilson.me.uk/post/31855915892/rules-of-static-linking-libstdc-libc-libgcc
    # http://insanecoding.blogspot.se/2012/07/creating-portable-linux-binaries.html
    QMAKE_LFLAGS += -static-libstdc++ -static-libgcc
    DESTDIR = build/lin
    OBJECTS_DIR = build/lin/obj
    MOC_DIR = build/lin/obj
    RCC_DIR = build/lin/obj
    UI_DIR = build/lin/obj
}

release_macos {
    # brew install qt
    DESTDIR = build/macos
    OBJECTS_DIR = build/macos/obj
    MOC_DIR = build/macos/obj
    RCC_DIR = build/macos/obj
    UI_DIR = build/macos/obj
}

release_android {
    DESTDIR = build/android
    OBJECTS_DIR = build/android/obj
    MOC_DIR = build/android/obj
    RCC_DIR = build/android/obj
    UI_DIR = build/android/obj
}

build_mobile {
    DEFINES += USE_MOBILE
}

SOURCES += main.cpp\
        mainwindow.cpp \
    packet.cpp \
    udpserversimple.cpp \
    vbytearray.cpp \
    commands.cpp \
    configparams.cpp \
    configparam.cpp \
    vescinterface.cpp \
    parametereditor.cpp \
    digitalfiltering.cpp \
    setupwizardapp.cpp \
    setupwizardmotor.cpp \
    startupwizard.cpp \
    utility.cpp \
    tcpserversimple.cpp

HEADERS  += mainwindow.h \
    packet.h \
    udpserversimple.h \
    vbytearray.h \
    commands.h \
    datatypes.h \
    configparams.h \
    configparam.h \
    vescinterface.h \
    parametereditor.h \
    digitalfiltering.h \
    setupwizardapp.h \
    setupwizardmotor.h \
    startupwizard.h \
    utility.h \
    tcpserversimple.h

FORMS    += mainwindow.ui \
    parametereditor.ui

contains(DEFINES, HAS_BLUETOOTH) {
    SOURCES += bleuart.cpp
    HEADERS += bleuart.h
}

include(pages/pages.pri)
include(widgets/widgets.pri)
include(mobile/mobile.pri)
include(map/map.pri)
include(lzokay/lzokay.pri)

RESOURCES += res.qrc \
    res_fw_bms.qrc \
    res_qml.qrc
RESOURCES += res_config.qrc

build_vari {
    RESOURCES += res_vari.qrc \
    res_fw.qrc
    DEFINES += VER_ORIGINAL
}

DISTFILES += \
    android/AndroidManifest.xml \
    android/gradle/wrapper/gradle-wrapper.jar \
    android/gradlew \
    android/res/values/libs.xml \
    android/build.gradle \
    android/gradle/wrapper/gradle-wrapper.properties \
    android/src/com/vedder/vesc/VForegroundService.java \
    android/src/com/vedder/vesc/Utils.java

ANDROID_PACKAGE_SOURCE_DIR = $$PWD/android
