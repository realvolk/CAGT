# Maintainer: realvolk <realvolk@github.com>
pkgname=cagt-git
pkgver=1.3.1.r0.g$(git rev-parse --short HEAD 2>/dev/null || echo 0)
pkgrel=1
pkgdesc="Single-file TUI AMD GPU fan controller with daemon mode"
arch=('x86_64')
url="https://github.com/realvolk/CAGT"
license=('custom:IRX')
depends=('ncurses')
makedepends=('git' 'gcc')
source=("git+https://github.com/realvolk/CAGT.git")
sha256sums=('SKIP')

pkgver() {
    cd "$srcdir/CAGT"
    git describe --long --tags | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g'
}

build() {
    cd "$srcdir/CAGT"
    gcc -std=c99 -O2 -o cagt cagt.c -lncurses
}

package() {
    cd "$srcdir/CAGT"
    install -Dm755 cagt "$pkgdir/usr/bin/cagt"
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
}