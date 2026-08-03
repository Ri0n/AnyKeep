pkgname=anykeep
pkgver=3.2.0
pkgrel=1
pkgdesc="Note-taking application written with Qt in mind"
arch=('i686' 'x86_64')
url="https://anykeep.net"
license=('GPL3')
depends=( 'qt5-base' 'qt5-tools' 'hunspell'
          'kglobalaccel5' 'kwindowsystem5'
          'knotifications5')
makedepends=('tar')
conflicts=(anykeep-git)
source=(https://github.com/Ri0n/AnyKeep/releases/download/$pkgver/$pkgname-$pkgver.tar.xz)
md5sums=('9402cdef96dc0bcb41c66d00948d5aa5')

_srcdirname=AnyKeep

build() {
  cd "$srcdir"
  # BUILD HERE
  cd "$srcdir/$_srcdirname-$pkgver"
  mkdir -p build; cd build
  cmake -DCMAKE_INSTALL_PREFIX="/usr" ..
  cmake --build . --target all
}

package() {
  cd "$srcdir/$_srcdirname-$pkgver/build"
  make DESTDIR="$pkgdir"  install
}
