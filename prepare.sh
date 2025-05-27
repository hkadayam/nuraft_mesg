set -eu

conan_major=`conan --version | awk '{print $3}' | awk -F'.' '{print $1}'`
echo "Using conan major version: ${conan_major}"

echo -n "Exporting custom recipes..."
echo -n "nuraft."

if [ $conan_major -eq 1 ] ; then
    conan export 3rd_party/nuraft nuraft/2.4.5@
else
    conan export 3rd_party/nuraft --name nuraft --version 2.4.5 >/dev/null
fi
echo "done."
