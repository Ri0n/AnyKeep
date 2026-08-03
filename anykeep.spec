%if 0%{?fedora}
%define _pkgconfig pkgconfig
%define _qt5linguist qt5-linguist
%define _qt5basedevel qt5-qtbase-devel
%else
%define qmake_qt5 qmake-qt5
%define _pkgconfig pkg-config
%define _qt5linguist  libqt5-linguist
%define _qt5basedevel libqt5-qtbase-devel
%endif

Name:           anykeep
Version:        3.2.0
Release:        1%{?dist}
Summary:        Simple note-taking application in Qt
# FIXME: Select a correct license from https://github.com/openSUSE/spec-cleaner#spdx-licenses
License:        GPLv3+
# FIXME: use correct group, see "https://en.opensuse.org/openSUSE:Package_group_guidelines"
Group:          Productivity/Office/Other
Url:            https://anykeep.net
Source:         https://github.com/Ri0n/AnyKeep/releases/download/%{version}/%{name}-%{version}.tar.gz
Requires:       libhunspell >= 1.6
Requires:       libQt5Widgets
BuildRequires:  gcc-c++
BuildRequires:  %{_pkgconfig}
BuildRequires:  %{_qt5linguist}
BuildRequires:  %{_qt5basedevel}
#BuildRequires:  qtsingleapplication-devel # unfortunately no in suse repos
BuildRequires:  hunspell-devel
BuildRequires:  kwindowsystem-devel
BuildRequires:  knotifications-devel
BuildRequires:  kglobalaccel-devel

%debug_package

%description
Note-taking application written with Qt in mind.
It's mostly Tomboy clone but with less features. So it's app
which lives in your system tray and allows you to make notes quickly

%package        devel
Summary:        AnyKeep development files
Group:          Development/Libraries
Requires:       %{name} = %{version}-%{release}

%description devel
This package contains headers and libraries required to build applications
using libanykeep and also to build AnyKeep plugins.

%prep
%autosetup -p1 -n %{name}-%{version}

%build
%if 0%{?fedora}
%{cmake} -DCMAKE_INSTALL_PREFIX=%{_prefix} -DCMAKE_BUILD_TYPE=Release LIBDIR=%{_libdir} -DQMAKE_CXXFLAGS="%{optflags}" -DQMAKE_CFLAGS="%{optflags}" .
%else
%{cmake} -DCMAKE_INSTALL_PREFIX=%{_prefix} -DCMAKE_BUILD_TYPE=Release LIBDIR=%{_libdir} -DQMAKE_CXXFLAGS="%{optflags}" -DQMAKE_CFLAGS="%{optflags}" .
%endif
%make_build

%install
make install INSTALL_ROOT=%{buildroot}

%files
%{_bindir}/%{name}
%{_libdir}/%{name}
%{_libdir}/libanykeep.so.*
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor
%{_datadir}/%{name}/*.qm
%{_mandir}/man1/%{name}.1.gz
%license license.txt
%doc Changelog README.md

%files devel
%{_prefix}/include
%{_libdir}/libanykeep.so
%{_datadir}/%{name}/common.pri
%{_datadir}/%{name}/plugins/plugin.pri

%changelog
* Sun Nov 18 2018 Sergey Ilinykh <rion4ik@gmail.com> -  3.0.5-1
- First rpm build
