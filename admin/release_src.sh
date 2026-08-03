#!/bin/bash

set -ex

#git clean -xfd .
#git sudmofule foreach git clean -xfd .


gittag2version() {
  # remove hash and sum to last numbers. e.g. v1.0.5-35-gdfsdfggw => v1.0.40
  # (so please avoid using anything but 0 in the patch part of version)
  local git_version=$(echo "$1" | sed 's/-g[a-z0-9]\{7,\}//')
  gv_lp=${git_version##*.}
  case "$gv_lp" in
    *-*) build_v=$(expr ${gv_lp%-*} + ${gv_lp#*-}) ;;
    *) build_v="$gv_lp" ;;
  esac
  echo "${git_version%.*}.$build_v"
}

version=$(gittag2version ${1:-$(git describe --tags 2>/dev/null)})
base_name=anykeep-$version
release_dir=../${base_name}
tar_file=${release_dir}.tar
git archive -o $tar_file HEAD
git submodule --quiet foreach 'cd $toplevel; tar -rf '"$tar_file"' $sm_path'

rm -rf $release_dir
mkdir $release_dir
tar -C ../${base_name} -xf $tar_file

echo -n "$version" > ${release_dir}/anykeep.version
sed -i -e "s|^pkgver=.*|pkgver=$version|" ${release_dir}/PKGBUILD
sed -i -e "s|^\(Version:\s*\)[0-9].*|\1$version|" ${release_dir}/anykeep.spec
sed -i -e "s| Version=\"[^\"]\+\"| Version=\"$version\"|" ${release_dir}/installer.wxs
sed -i -e "1 s|anykeep ([^)]\+)|anykeep ($version)|" ${release_dir}/debian/changelog

tar cJvf $tar_file.xz $release_dir
echo "result: $tar_file.xz"
