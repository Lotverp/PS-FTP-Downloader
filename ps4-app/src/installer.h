#ifndef INSTALLER_H
#define INSTALLER_H

#include <stdbool.h>
#include <stddef.h>

bool installer_install_pkg(const char *pkg_path, char *out_msg, size_t msg_size);

#endif /* INSTALLER_H */
