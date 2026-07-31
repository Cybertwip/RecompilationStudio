#include "vwine/vwine_package.h"
#include "vwine/vwine_log.h"

#include <stdio.h>
#include <string.h>

// The names Studio stages into a package. They are fixed by the exporter
// (studio/src/GuestAppSupport.cpp, stageGuestApp) rather than chosen here, and
// the generated CMake copies them beside the runtime under exactly these names.
#define VWINE_PACKAGE_IMAGE "executable"
#define VWINE_PACKAGE_DATA  "data"

// MVII's filesystem paths use '/'; '\\' is accepted too so that a path which
// came from a FAT tool is not silently treated as having no directory at all.
static bool vwine_is_separator(char c)
{
    return c == '/' || c == '\\';
}

bool vwine_package_dir(const char* argv0, char* out, size_t out_size)
{
    if (!argv0 || !argv0[0] || !out || out_size == 0) return false;

    size_t last = 0;
    bool found = false;
    for (size_t i = 0; argv0[i]; ++i) {
        if (vwine_is_separator(argv0[i])) { last = i; found = true; }
    }
    // No separator means argv[0] is a bare name, so there is no directory in it
    // to resolve against. Refusing is right: the alternative is to guess the
    // current working directory, and a wrong guess here loads the wrong guest.
    if (!found) return false;

    // A path that is just "/" keeps its separator, because dropping it would
    // turn an absolute path into an empty one.
    const size_t length = (last == 0) ? 1 : last;
    if (length + 1 > out_size) return false;

    memcpy(out, argv0, length);
    out[length] = '\0';
    return true;
}

bool vwine_package_path(const char* argv0, const char* relative, char* out,
                        size_t out_size)
{
    if (!relative || !relative[0]) return false;
    if (!vwine_package_dir(argv0, out, out_size)) return false;

    size_t used = strlen(out);
    const bool need_separator = used > 0 && !vwine_is_separator(out[used - 1]);
    const size_t relative_length = strlen(relative);
    if (used + (need_separator ? 1u : 0u) + relative_length + 1u > out_size) return false;

    if (need_separator) out[used++] = '/';
    memcpy(out + used, relative, relative_length);
    out[used + relative_length] = '\0';
    return true;
}

bool vwine_package_file_exists(const char* path)
{
    if (!path || !path[0]) return false;
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    fclose(fp);
    return true;
}

bool vwine_package_image(const char* argv0, char* out, size_t out_size)
{
    if (!vwine_package_path(argv0, VWINE_PACKAGE_IMAGE, out, out_size)) return false;
    return vwine_package_file_exists(out);
}

bool vwine_package_data_root(const char* argv0, char* out, size_t out_size)
{
    return vwine_package_path(argv0, VWINE_PACKAGE_DATA, out, out_size);
}

bool vwine_package_resolve_image(const char* front_end, int argc, char** argv,
                                 char* image_out, size_t image_out_size,
                                 char* data_out, size_t data_out_size)
{
    if (!front_end) front_end = "virtua";
    if (!image_out || image_out_size == 0) return false;

    const char* argv0 = (argv && argv[0]) ? argv[0] : "";

    // The data root is reported whether or not the image came from the command
    // line, because a module pointed at by hand during bring-up still sits in a
    // staged package and still wants that package's data.
    if (data_out && data_out_size > 0) {
        if (!vwine_package_data_root(argv0, data_out, data_out_size)) data_out[0] = '\0';
    }

    if (argc >= 2 && argv && argv[1] && argv[1][0]) {
        const size_t given = strlen(argv[1]);
        if (given + 1 > image_out_size) {
            vwine_logf("%s: the image path is longer than this front-end can hold "
                       "(%u bytes)\n", front_end, (unsigned)image_out_size);
            return false;
        }
        memcpy(image_out, argv[1], given + 1);
        return true;
    }

    // No path given, which is what being launched from the dashboard looks
    // like. The guest is the `executable` Studio staged beside this `.virtua`.
    if (!argv0[0]) {
        vwine_logf("%s: launched with no image path and no argv[0], so there is "
                   "nothing to resolve the package against\n", front_end);
        return false;
    }

    char package[512];
    if (!vwine_package_dir(argv0, package, sizeof(package))) {
        vwine_logf("%s: launched with no image path, and argv[0] (\"%s\") names no "
                   "directory to find the package in\n", front_end, argv0);
        return false;
    }

    if (!vwine_package_path(argv0, VWINE_PACKAGE_IMAGE, image_out, image_out_size)) {
        vwine_logf("%s: the package path under %s does not fit in %u bytes\n",
                   front_end, package, (unsigned)image_out_size);
        return false;
    }
    if (!vwine_package_file_exists(image_out)) {
        vwine_logf("%s: this package has no guest to run. %s holds no readable "
                   "`%s`, which is the file the export stages the guest module "
                   "as. The package was built without it or it did not survive "
                   "delivery.\n",
                   front_end, package, VWINE_PACKAGE_IMAGE);
        return false;
    }

    vwine_logf("%s: no image on the command line; using the package's %s\n",
               front_end, VWINE_PACKAGE_IMAGE);
    return true;
}
