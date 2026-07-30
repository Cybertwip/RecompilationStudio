#include "GuestAppSupport.h"

#include "PipelineSupport.h"

#include "quazip.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace psxstudio {

namespace {

// Vita.
constexpr quint16 kElfTypeSceRelexec = 0xFE04u;  // ET_SCE_RELEXEC, sce-elf.h
constexpr quint16 kElfMachineArm = 40u;          // EM_ARM
// Horizon.
constexpr quint32 kNsoMagic = 0x304F534Eu;  // 'NSO0' little-endian
constexpr quint32 kNroMagic = 0x304F524Eu;  // 'NRO0'
constexpr quint32 kPfs0Magic = 0x30534650u; // 'PFS0'

quint16 readLe16(const QByteArray& bytes, qsizetype offset) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
  return static_cast<quint16>(p[0]) | static_cast<quint16>(p[1] << 8u);
}

quint32 readLe32(const QByteArray& bytes, qsizetype offset) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
  return static_cast<quint32>(p[0]) |
         (static_cast<quint32>(p[1]) << 8u) |
         (static_cast<quint32>(p[2]) << 16u) |
         (static_cast<quint32>(p[3]) << 24u);
}

quint64 readLe64(const QByteArray& bytes, qsizetype offset) {
  return static_cast<quint64>(readLe32(bytes, offset)) |
         (static_cast<quint64>(readLe32(bytes, offset + 4)) << 32u);
}

/* horizon_image.c's arch_from_entry_branch, applied to the same first word.
 * AArch64 is tested first for the reason stated there: the ARM32 test is a byte
 * compare an AArch64 branch could in principle satisfy. */
GuestArch archFromEntryBranch(quint32 word) {
  if ((word >> 26u) == 0x05u) return GuestArch::AArch64;  // AArch64 B
  if ((word >> 24u) == 0xEAu) return GuestArch::Arm32;    // ARM32 B, cond=AL
  return GuestArch::Unknown;
}

/* The first four decompressed bytes of an LZ4 block, when they can be had
 * without decoding it.
 *
 * horizon2mvii decodes NSO segments properly; duplicating its decoder here to
 * read one word would be two implementations of the same format, and the second
 * one is the one nothing tests. But an LZ4 block always opens with a literal
 * run — a block cannot begin with a match, there is nothing behind the cursor to
 * match against — so when that run is at least four bytes long, the first four
 * decompressed bytes are just the first four literals.
 *
 * When the run is shorter, this reports failure rather than guessing, and the
 * caller reports GuestArch::Unknown. Studio does not need to be sure here: the
 * front-end's own two-signal probe is what decides on the device. */
bool lz4FirstWord(const QByteArray& block, quint32& word) {
  if (block.isEmpty()) return false;
  const auto* bytes = reinterpret_cast<const uchar*>(block.constData());
  qsizetype cursor = 0;
  quint64 literals = static_cast<quint64>(bytes[cursor++] >> 4u);
  if (literals == 15u) {
    for (;;) {
      if (cursor >= block.size()) return false;
      const uchar add = bytes[cursor++];
      literals += add;
      if (add != 255u) break;
      if (literals > static_cast<quint64>(block.size())) return false;
    }
  }
  if (literals < 4u || cursor + 4 > block.size()) return false;
  word = readLe32(block, cursor);
  return true;
}

QString containerDisplayName(GuestContainer container) {
  switch (container) {
    case GuestContainer::VitaElf:     return QStringLiteral("ARM ELF32 (ET_SCE_RELEXEC)");
    case GuestContainer::VitaSelf:    return QStringLiteral("SELF");
    case GuestContainer::VitaVpk:     return QStringLiteral("VPK");
    case GuestContainer::VitaPkg:     return QStringLiteral("PKG");
    case GuestContainer::HorizonNro:  return QStringLiteral("NRO0");
    case GuestContainer::HorizonNso:  return QStringLiteral("NSO0");
    case GuestContainer::HorizonNsp:  return QStringLiteral("PFS0 (NSP)");
    case GuestContainer::HorizonXci:  return QStringLiteral("XCI");
    case GuestContainer::Unknown:     break;
  }
  return QStringLiteral("unrecognized");
}

/* PFS0's own table, walked to name what is inside without decrypting any of it:
 * a u32 entry count and u32 string-table size at 0x08, then `count` 0x18-byte
 * entries, then the string table. */
QStringList pfs0Entries(const QByteArray& head, qint64& largestEntry) {
  QStringList names;
  largestEntry = 0;
  if (head.size() < 0x10) return names;
  const quint32 count = readLe32(head, 0x04);
  const quint32 stringTableSize = readLe32(head, 0x08);
  if (count == 0u || count > 4096u) return names;
  const qsizetype tableAt = 0x10;
  const qsizetype stringsAt = tableAt + static_cast<qsizetype>(count) * 0x18;
  if (stringsAt + static_cast<qsizetype>(stringTableSize) > head.size()) return names;
  for (quint32 index = 0; index < count; ++index) {
    const qsizetype entry = tableAt + static_cast<qsizetype>(index) * 0x18;
    const qint64 size = static_cast<qint64>(readLe64(head, entry + 0x08));
    const quint32 nameOffset = readLe32(head, entry + 0x10);
    if (nameOffset >= stringTableSize) continue;
    const QByteArray name = QByteArray(head.constData() + stringsAt + nameOffset);
    largestEntry = std::max(largestEntry, size);
    names.append(QStringLiteral("%1 (%2 bytes)")
                   .arg(QString::fromLatin1(name)).arg(size));
  }
  return names;
}

void inspectVita(const QByteArray& head, const QString& path,
                 GuestAppDescription& description) {
  // Order follows load_executable(): ELF magic, then SCE magic, then the VPK
  // fallback. A file matching none of the three is what that function's final
  // `return -1` refuses.
  if (head.size() >= 20 && head.startsWith(QByteArrayLiteral("\x7f" "ELF"))) {
    const quint16 type = readLe16(head, 16);
    const quint16 machine = head.size() >= 20 ? readLe16(head, 18) : 0u;
    description.container = GuestContainer::VitaElf;
    description.arch = GuestArch::Arm32;
    if (static_cast<uchar>(head.at(4)) != 1u) {  // ELFCLASS32
      description.arch = GuestArch::AArch64;
      description.refusal = QStringLiteral(
        "This is a 64-bit ELF. The Vita is a 32-bit ARMv7-A Cortex-A9 and so is "
        "the MVII device; there is no 64-bit Vita executable to run.");
      return;
    }
    if (machine != kElfMachineArm) {
      description.arch = GuestArch::Unknown;
      description.refusal = QStringLiteral(
        "This ELF is not built for ARM (e_machine %1). vita2mvii branches into "
        "the guest's own instructions, so they have to be ARM instructions.")
          .arg(machine);
      return;
    }
    if (type != kElfTypeSceRelexec) {
      description.refusal = QStringLiteral(
        "This is a plain ARM ELF (e_type 0x%1), not a Vita module. vita2mvii "
        "loads ET_SCE_RELEXEC (0xFE04) — a .velf or the eboot.elf a Vita "
        "toolchain emits — because that is the form that carries the SCE "
        "relocations and the import table it binds.")
          .arg(type, 4, 16, QLatin1Char('0'));
    }
    return;
  }

  if (head.size() >= 4 && head.startsWith(QByteArrayLiteral("SCE\0"))) {
    description.container = GuestContainer::VitaSelf;
    description.arch = GuestArch::Arm32;
    if (head.size() < 0x20) {
      description.refusal = QStringLiteral("The SELF header is truncated.");
      return;
    }
    const quint32 version = readLe32(head, 0x04);
    const quint16 headerType = readLe16(head, 0x0A);
    if (version != 3u) {
      description.refusal = QStringLiteral(
        "SELF header version %1 is not supported; vita2mvii loads version 3.")
          .arg(version);
      return;
    }
    if (headerType != 1u) {
      // header_type 3 is the SELF-inside-a-PKG form.
      description.refusal = QStringLiteral(
        "SELF header type %1 is not a plain self (type 1). vita2mvii does not "
        "decrypt, so it loads only an unencrypted SELF.")
          .arg(headerType);
    }
    return;
  }

  if (head.size() >= 4 && head.startsWith(QByteArrayLiteral("\x7f" "PKG"))) {
    description.container = GuestContainer::VitaPkg;
    description.refusal = QStringLiteral(
      "This is a PSN package (PKG). Its contents are encrypted and the key is "
      "in a licence (a zRIF / work.bin) that is not part of the file, so there "
      "is nothing here vita2mvii could load — it does not decrypt, by design. "
      "Extract the title to a VPK, or supply the unencrypted eboot.bin / .velf.");
    return;
  }

  if (head.size() >= 4 && head.startsWith(QByteArrayLiteral("PK\x03\x04"))) {
    description.container = GuestContainer::VitaVpk;
    description.arch = GuestArch::Arm32;
    QuaZip archive(path);
    if (!archive.open(QuaZip::mdUnzip)) {
      description.refusal = QStringLiteral(
        "This is a ZIP, but it could not be opened to confirm it is a VPK.");
      return;
    }
    bool hasEboot = false;
    for (bool more = archive.goToFirstFile(); more; more = archive.goToNextFile()) {
      const QString name = archive.getCurrentFileName();
      description.contents.append(name);
      if (name.compare(QStringLiteral("eboot.bin"), Qt::CaseSensitive) == 0) hasEboot = true;
    }
    archive.close();
    if (!hasEboot) {
      // load_vpk extracts by exact name, so a nested or differently-cased entry
      // is a miss there too.
      description.refusal = QStringLiteral(
        "This ZIP has no eboot.bin at its root. vita2mvii extracts that exact "
        "entry from a VPK and loads it as the module.");
    }
    return;
  }

  description.refusal = QStringLiteral(
    "This file is not an ARM ELF, a SELF or a VPK, which are the three "
    "containers vita2mvii opens.");
}

void inspectHorizon(const QByteArray& head, GuestAppDescription& description) {
  if (head.size() >= 4 && readLe32(head, 0) == kNsoMagic) {
    description.container = GuestContainer::HorizonNso;
    if (head.size() < 0x100) {
      description.refusal = QStringLiteral("The NSO0 header is truncated.");
      return;
    }
    // .text is segment 0; flags bit 0 says whether its bytes are LZ4 blocks.
    const quint32 flags = readLe32(head, 0x0C);
    const quint32 textOffset = readLe32(head, 0x10);
    const quint32 textCompressed = readLe32(head, 0x60);
    quint32 entryWord = 0;
    bool haveEntry = false;
    if ((flags & 1u) == 0u) {
      if (textOffset + 4u <= static_cast<quint32>(head.size())) {
        entryWord = readLe32(head, textOffset);
        haveEntry = true;
      }
    } else if (textOffset < static_cast<quint32>(head.size())) {
      const qsizetype available =
        std::min<qsizetype>(head.size() - textOffset, textCompressed);
      haveEntry = lz4FirstWord(head.mid(textOffset, available), entryWord);
    }
    if (haveEntry) description.arch = archFromEntryBranch(entryWord);
  } else if (head.size() >= 0x14 && readLe32(head, 0x10) == kNroMagic) {
    description.container = GuestContainer::HorizonNro;
    description.arch = archFromEntryBranch(readLe32(head, 0x00));
  } else if (head.size() >= 4 && readLe32(head, 0) == kPfs0Magic) {
    description.container = GuestContainer::HorizonNsp;
    qint64 largest = 0;
    description.contents = pfs0Entries(head, largest);
    description.refusal = QStringLiteral(
      "This is an NSP: a PFS0 archive of NCAs. Every NCA in it is encrypted "
      "under Nintendo's title keys, which are not in the file, so horizon2mvii "
      "has nothing to load — it decodes NRO0 and NSO0 modules and does not "
      "decrypt. Supply the NRO or the NSO.");
    return;
  } else if (head.size() >= 0x104 &&
             head.mid(0x100, 4) == QByteArrayLiteral("HEAD")) {
    description.container = GuestContainer::HorizonXci;
    description.refusal = QStringLiteral(
      "This is an XCI cartridge dump. Like an NSP it carries encrypted NCAs, "
      "so horizon2mvii has nothing to load. Supply the NRO or the NSO.");
    return;
  } else {
    description.refusal = QStringLiteral(
      "This file is neither an NRO0 nor an NSO0, which are the two module "
      "containers horizon2mvii decodes.");
    return;
  }

  // Both module containers land here, and the architecture is what decides.
  if (description.arch == GuestArch::AArch64) {
    description.refusal = QStringLiteral(
      "This module is AArch64. The MVII device is an ARMv7-A Cortex-A7, which "
      "cannot decode a single AArch64 instruction — so this is a different "
      "project rather than a harder case. horizon2mvii runs 32-bit (AArch32) "
      "Horizon modules.");
  } else if (description.arch == GuestArch::Unknown) {
    // Not a refusal. Studio applies one of the front-end's two signals; the
    // other is .dynamic, which is only reachable after the module is decoded.
    // horizon2mvii asks both and refuses if they disagree.
    description.refusal.clear();
  }
}

}  // namespace

QString guestArchName(GuestArch arch) {
  switch (arch) {
    case GuestArch::Arm32:   return QStringLiteral("ARM32 (AArch32)");
    case GuestArch::AArch64: return QStringLiteral("AArch64");
    case GuestArch::Unknown: break;
  }
  return QStringLiteral("indeterminate");
}

bool inspectGuestApp(SystemKind system,
                     const QString& path,
                     GuestAppDescription& description,
                     QString& error) {
  description = {};
  description.system = system;
  if (!systemRunsGuestNatively(system)) {
    error = QStringLiteral("%1 is not a direct-execution system.")
              .arg(systemKindDisplayName(system));
    return false;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the %1 application: %2")
              .arg(systemKindDisplayName(system), file.errorString());
    return false;
  }
  description.size = file.size();
  if (description.size < 0x40) {
    error = QStringLiteral("The selected file is too small to hold a %1 container header.")
              .arg(systemKindDisplayName(system));
    return false;
  }
  // Only the head: identification reads headers, and an NSP is hundreds of
  // megabytes of ciphertext there is no reason to pull into memory. 1 MiB
  // covers every header here plus a PFS0 string table.
  const QByteArray head = file.read(1024 * 1024);
  file.close();
  if (head.isEmpty()) {
    error = QStringLiteral("Could not read the %1 application header.")
              .arg(systemKindDisplayName(system));
    return false;
  }

  if (system == SystemKind::Vita) inspectVita(head, path, description);
  else inspectHorizon(head, description);

  description.containerName = containerDisplayName(description.container);
  description.suggestedTitle = QFileInfo(path).completeBaseName();
  return true;
}

QString guestAppFileFilter(SystemKind system) {
  if (system == SystemKind::Vita) {
    return QStringLiteral(
      "Vita applications (*.self *.velf *.elf *.bin *.vpk);;All files (*)");
  }
  return QStringLiteral("Horizon modules (*.nro *.nso);;All files (*)");
}

bool scanGuestAppDirectory(SystemKind system,
                           const QString& directory,
                           QList<GuestCatalogEntry>& entries,
                           QStringList& warnings,
                           QString& error) {
  entries.clear();
  warnings.clear();
  if (!QFileInfo(directory).isDir()) {
    error = QStringLiteral("The selected %1 application directory does not exist.")
              .arg(systemKindDisplayName(system));
    return false;
  }
  const QStringList patterns = system == SystemKind::Vita
    ? QStringList{ QStringLiteral("*.self"), QStringLiteral("*.velf"),
                   QStringLiteral("*.vpk"), QStringLiteral("eboot.bin") }
    : QStringList{ QStringLiteral("*.nro"), QStringLiteral("*.nso") };
  QDirIterator iterator(directory, patterns, QDir::Files, QDirIterator::Subdirectories);
  QSet<QString> seen;
  while (iterator.hasNext()) {
    const QString path = iterator.next();
    const QString canonical = QFileInfo(path).canonicalFilePath();
    const QString key =
      (canonical.isEmpty() ? QFileInfo(path).absoluteFilePath() : canonical).toLower();
    if (seen.contains(key)) continue;
    seen.insert(key);
    GuestAppDescription description;
    QString inspectError;
    if (!inspectGuestApp(system, path, description, inspectError)) {
      warnings.append(QStringLiteral("%1: %2").arg(path, inspectError));
      continue;
    }
    // An image the front-end will not open is left out rather than queued to
    // fail on the device: a batch export exists to produce packages that run.
    if (!description.loadable()) {
      warnings.append(QStringLiteral("%1: %2").arg(path, description.refusal));
      continue;
    }
    entries.append({ path, description.suggestedTitle, description.containerName });
  }
  std::sort(entries.begin(), entries.end(),
            [](const GuestCatalogEntry& a, const GuestCatalogEntry& b) {
              return a.suggestedTitle.compare(b.suggestedTitle, Qt::CaseInsensitive) < 0;
            });
  return true;
}

QString generatedGuestAppProjectCMake(const PipelineRequest& request,
                                      const GuestAppDescription& app,
                                      const QString& bundleName) {
  Q_UNUSED(app);
  const bool vita = request.system == SystemKind::Vita;
  const QString frontEnd = systemFrontEndName(request.system);
  const QString subdirectory = vita ? QStringLiteral("vita2hos")
                                    : QStringLiteral("horizon2mvii");
  const QString prefix = vita ? QStringLiteral("VITA2MVII")
                              : QStringLiteral("HORIZON2MVII");
  const QString guestOs = vita ? QStringLiteral("the Vita's kernel and its module imports")
                               : QStringLiteral("Horizon's kernel, SVC layer and HIPC/CMIF IPC");
  return QStringLiteral(R"CMAKE(cmake_minimum_required(VERSION 3.20)
project(GeneratedGuestVirtuaPackage C CXX ASM)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# There is no host form of this package, and that is the design rather than a
# packaging gap: the guest's ARMv7 instructions run on the device's ARMv7 CPU
# untranslated. An x86_64 build would compile every line of the loader and then
# have nothing it could branch to. framework/extra/%2 refuses a non-ARM
# CMAKE_SYSTEM_PROCESSOR itself; it is said here too, where the mistake is made.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7)")
  message(FATAL_ERROR
    "This package targets MVII/Virtua. Configure with "
    "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/framework/extra/virtua/CMake/VirtuaArmToolchain.cmake "
    "-DPOWERENGINE_ROOT=<PowerEngine> -DVIRTUA_LLVM_ROOT=<compiler bundle>")
endif()

set(GUEST_APP_NAME %1)
set(_GUEST_SRC "${CMAKE_CURRENT_SOURCE_DIR}")
set(_GUEST_STAGE "${CMAKE_BINARY_DIR}/steganos-package/guest-runtime/$<CONFIG>")

# The guest image itself. Nothing was recompiled during export: %4 is what
# this package supplies, and the guest's own code is loaded, relocated and
# branched to. Its absence is a hard error — an image-less package would build
# and then exit on argv[1].
set(GUEST_APP_IMAGE "${_GUEST_SRC}/package_inputs/executable")
if(NOT EXISTS "${GUEST_APP_IMAGE}")
  message(FATAL_ERROR
    "package_inputs/executable is missing. Re-export from PSXRecomp Studio; "
    "this package carries the guest image, it does not fetch one.")
endif()

# Plain variables, read by framework/extra/%2/CMakeLists.txt before it defines
# anything. %3_PAYLOAD is what makes it stage the image beside the runtime as
# `executable`, which is the path the front-end reads from argv[1].
set(%3_OUTPUT_NAME "${GUEST_APP_NAME}")
set(%3_PAYLOAD "${GUEST_APP_IMAGE}")
set(%3_STAGE_DIR "${_GUEST_STAGE}")
add_subdirectory(framework/extra/%2 "${CMAKE_BINARY_DIR}/%2")

# The rest of what a Steganos package carries.
add_custom_target(guest-runtime ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_GUEST_SRC}/AppIcon.png" "${_GUEST_STAGE}/AppIcon.png"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_GUEST_SRC}/game.manifest.json" "${_GUEST_STAGE}/game.manifest.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_GUEST_SRC}/PSXRecomp-Proof.zip" "${_GUEST_STAGE}/PSXRecomp-Proof.zip"
  DEPENDS "${_GUEST_SRC}/AppIcon.png"
          "${_GUEST_SRC}/game.manifest.json"
          "${_GUEST_SRC}/PSXRecomp-Proof.zip"
  COMMENT "Staging the ${GUEST_APP_NAME} Virtua package"
  VERBATIM)
add_dependencies(guest-runtime %5-stage)
)CMAKE")
    .arg(cmakeQuoted(bundleName), subdirectory, prefix, guestOs, frontEnd);
}

QString generatedGuestAppReadme(const PipelineRequest& request,
                                const GuestAppDescription& app,
                                const QString& bundleName) {
  const bool vita = request.system == SystemKind::Vita;
  const QString frontEnd = systemFrontEndName(request.system);
  const QString subdirectory = vita ? QStringLiteral("vita2hos")
                                    : QStringLiteral("horizon2mvii");
  const QString guestCpu = vita ? QStringLiteral("ARMv7-A Cortex-A9")
                                : QStringLiteral("ARMv7-A (AArch32)");
  const QString guestOs = vita
    ? QStringLiteral(
        "the Vita's kernel API. Every import the module names is bound to a "
        "host implementation, and an import with no implementation is a hard "
        "failure reported by name — never a stub that returns 0 and misbehaves "
        "somewhere else.")
    : QStringLiteral(
        "Horizon's kernel, SVC layer and HIPC/CMIF IPC. An unimplemented SVC "
        "prints its register frame and stops rather than returning a Result "
        "for a service that did not run.");
  return QStringLiteral(
    "# %1 — %2 Studio export\n\n"
    "Generated by PSXRecomp Studio for **Virtua ARM**.\n\n"
    "## Nothing here was recompiled\n\n"
    "The %3 is %4 and the MVII device is an ARMv7-A Cortex-A7. Same instruction "
    "set — so the guest's own code runs on the CPU untranslated, and there is no "
    "translation step to perform, inspect or verify. What is missing on the "
    "device is not a CPU, it is the guest's operating system, and that is what "
    "`framework/extra/%5` supplies: %6\n\n"
    "This is the WINE model, not emulation, and it is why this package exists "
    "for Virtua ARM alone. A macOS, Windows or Linux build would be an x86_64 "
    "loader with nothing it could branch to.\n\n"
    "## Layout\n\n"
    "- `package_inputs/executable` — the guest image, exactly as selected: a %7 "
    "of %8 bytes. It is staged beside the runtime under that same name, which is "
    "the path `%2` reads from `argv[1]`.\n"
    "- `framework/extra/%5/` — the front-end: the loader, the relocator and the "
    "guest-OS reimplementation.\n"
    "- `framework/extra/virtua/` — the shared guest-OS layer (`virtua-wine`: the "
    "ARM ELF32 mapper, the guest memory mapper and the missing-import registry) "
    "and the CMake that resolves PowerEngine. Nothing from PowerEngine is "
    "vendored; the toolchain, sysroot, R-Dash, linker script and packager all "
    "come out of `POWERENGINE_ROOT`.\n"
    "- `proof/` — the container identification and the packaged-image verification.\n\n"
    "## Build\n\n"
    "```sh\ncmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \\\n"
    "  -DCMAKE_TOOLCHAIN_FILE=framework/extra/virtua/CMake/VirtuaArmToolchain.cmake \\\n"
    "  -DPOWERENGINE_ROOT=/path/to/PowerEngine \\\n"
    "  -DVIRTUA_LLVM_ROOT=/path/to/PowerEngine/compiler-bundle\n"
    "cmake --build build --target guest-runtime --parallel\n```\n\n"
    "The package is emitted under `build/steganos-package/guest-runtime/Release/`: "
    "`%9.virtua` with `executable` beside it.\n")
      .arg(request.windowTitle, frontEnd, systemKindDisplayName(request.system),
           guestCpu, subdirectory, guestOs, app.containerName,
           QString::number(app.size), bundleName);
}

} // namespace psxstudio
