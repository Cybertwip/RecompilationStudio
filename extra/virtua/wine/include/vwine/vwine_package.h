// vwine_package.h — finding the guest inside the package the front-end was
// launched from.
//
// MVII launches a Virtua application by its `.virtua` file and passes that path
// as argv[0]. It passes nothing else: argc is 1. So a front-end that reads its
// guest image out of argv[1] can never be started from the dashboard -- it will
// print its usage line and exit, which is exactly what
// `vita2mvii: usage: ...` and `horizon2mvii: usage: ...` in the device console
// mean. Selecting the application on the device is not a special case to be
// handled; it is the only way an application is ever started there.
//
// The guest is not inside the `.virtua`. Studio stages a package directory and
// the `.virtua` is one file in it, beside the guest:
//
//     Undertale-Virtua-ARM/
//       Undertale.virtua      <- argv[0]; the front-end ELF, wrapped
//       executable            <- the guest module the front-end loads
//       data/                 <- the rest of the title (ExeFS + RomFS, or the
//                                decrypted Vita title), staged whole
//       game.manifest.json, README.md, AppIcon.png, PSXRecomp-Proof.zip
//
// So the guest image is `dirname(argv[0]) + "/executable"`, and the guest's
// data root is `dirname(argv[0]) + "/data"`. Both front-ends need the same two
// answers, which is why this is here and not copied into each of them.
//
// An explicit path on the command line still wins, so a module can be pointed
// at directly during bring-up. This only supplies the answer when no path was
// given -- the case the dashboard always produces.

#ifndef VWINE_PACKAGE_H
#define VWINE_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The directory holding argv[0], without its trailing separator. False when
// `argv0` is empty, has no separator (so there is no directory to name), or is
// longer than `out_size`.
bool vwine_package_dir(const char* argv0, char* out, size_t out_size);

// `<package dir>/<relative>`. False for the same reasons.
bool vwine_package_path(const char* argv0, const char* relative, char* out,
                        size_t out_size);

// True when `path` names a file that can be opened for reading. This is how the
// resolvers below decide, rather than assuming the stage is complete: a package
// missing its `executable` is a real thing that a failed export can produce, and
// the difference between "not there" and "there but unreadable" is not one this
// layer can act on differently.
bool vwine_package_file_exists(const char* path);

// The staged guest module, `<package dir>/executable`. False when it is not
// there; the caller reports, because only it knows what the module is called on
// its own system.
bool vwine_package_image(const char* argv0, char* out, size_t out_size);

// The staged data root, `<package dir>/data`. Reported without checking that it
// exists, because a title with no data is legitimate and the caller decides
// whether an absent root matters to it.
bool vwine_package_data_root(const char* argv0, char* out, size_t out_size);

// The whole of the argv convention both front-ends share, in one call.
//
// `argc`/`argv` are as main() received them. When an image path was given on
// the command line it is returned unchanged. When none was, the package is
// consulted. On failure this logs the reason -- naming the directory it looked
// in and the file it wanted -- and returns false, so callers need only choose an
// exit code.
//
// `front_end` is the name to log under ("vita2mvii", "horizon2mvii").
// `image_out` receives the resolved path; `data_out` may be NULL if the caller
// has no use for the data root yet.
bool vwine_package_resolve_image(const char* front_end, int argc, char** argv,
                                 char* image_out, size_t image_out_size,
                                 char* data_out, size_t data_out_size);

#ifdef __cplusplus
}
#endif

#endif  // VWINE_PACKAGE_H
