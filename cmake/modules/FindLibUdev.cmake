# FindLibUdev
# --------
#
# Find LibUdev
#
# Find the native LibUdev includes and library This module defines
#
# ::
#
#   LIBUDEV_INCLUDE_DIR, where to find libudev.h, etc.
#   LIBUDEV_LIBRARIES, the libraries needed to use LibUdev.
#   LIBUDEV_FOUND, If false, do not try to use LibUdev.
#
# also defined, but not for general use are
#
# ::
#
#   LIBUDEV_LIBRARY, where to find the LibUdev library.

find_path (LIBUDEV_INCLUDE_DIR libudev.h)

set (LIBUDEV_NAMES ${LIBUDEV_NAMES} udev)
find_library (LIBUDEV_LIBRARY NAMES ${LIBUDEV_NAMES})

# handle the QUIETLY and REQUIRED arguments and set LIBUDEV_FOUND to TRUE if
# all listed variables are TRUE
include (FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS (LIBUDEV DEFAULT_MSG LIBUDEV_LIBRARY LIBUDEV_INCLUDE_DIR)

if (LIBUDEV_FOUND)
  set (LIBUDEV_LIBRARIES ${LIBUDEV_LIBRARY})
endif (LIBUDEV_FOUND)

mark_as_advanced (LIBUDEV_LIBRARY LIBUDEV_INCLUDE_DIR)
