#pragma once

#include "PipelineTypes.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QVector>

namespace psxstudio {

struct GbaDescription {
  QString title;
  QString gameCode;
  QString makerCode;
  QString saveType;
  quint32 entryWord{ 0 };
  quint32 entryTarget{ 0x08000000u };
  qint64 romSize{ 0 };
  bool headerBranchValid{ false };
  bool complementValid{ false };
  QString warning;
};

struct GbaCatalogEntry {
  QString sourcePath;
  QString suggestedTitle;
  QString gameCode;
};

bool inspectGbaRom(const QString& path, GbaDescription& description, QString& error);
bool scanGbaDirectory(const QString& directory,
                      QList<GbaCatalogEntry>& entries,
                      QStringList& warnings,
                      QString& error);
QString sha1File(const QString& path, QString& error);
QString generatedGbaMainCpp(const QString& bundleName,
                            const QString& romSha1,
                            quint32 romCrc32);
QString generatedGbaGameToml(const PipelineRequest& request,
                             const GbaDescription& game,
                             const QString& romSha1,
                             quint32 romCrc32,
                             const QString& biosSha1,
                             quint32 biosCrc32,
                             bool biosHle);
// Per-binary configuration for the `gba_recompile` tool. Distinct from
// generatedGbaGameToml, which configures the runtime that ships in the package.
QString generatedGbaRecompilerToml(const PipelineRequest& request,
                                   const GbaDescription& game,
                                   const QString& romSha1);
QString generatedGbaProjectCMake(const PipelineRequest& request,
                                 const GbaDescription& game,
                                 const QString& bundleName,
                                 const QString& bundleId);

QString generatedGbaInfoPlist(const QString& bundleName,
                              const QString& bundleId);

// ── Ghidra seed admission ────────────────────────────────────────────────────
//
// A cartridge image has no section table, so Ghidra disassembles compressed art
// as willingly as it disassembles code, and its reference classes inherit that
// mistake: a THUMB BL second halfword with no first halfword in front of it
// becomes a call through LR with a propagated destination, manufacturing a
// COMPUTED_CALL out of data. Seeding on those admits art, and because each seed
// extends the translated extent the next pass admits more — a cascade.
//
// So admission asks gba_recompile, not Ghidra, what the cited address holds.

/* One gbarecomp-translated function's guest extent, read out of the header
 * comment gba_recompile writes above every lowered function in a shard:
 *     / * 0x080000C0  mode=arm  end=0x080000E0  branches=3  indirect * /
 * `end` is exclusive. */
struct GbaTranslatedSpan {
  quint32 start{ 0 };
  quint32 end{ 0 };
  bool thumb{ false };
  quint32 reach{ 0 };  // max `end` over this span and every earlier one
};

/* gba_recompile's own disassembly, keyed by address, from the one comment it
 * writes per lowered instruction:
 *     / * 0802C854  0802c854 T bl.lo 0x00000000 * / */
using GbaInstructionMap = QHash<quint32, QString>;

/* One Ghidra function offered as a possible seed, reduced to the evidence the
 * admission rules weigh. The source lists are kept apart because Ghidra sorts
 * them, not because that sorting is trusted. */
struct GbaSeedCandidate {
  quint32 entry{ 0 };
  bool thumb{ false };
  QString name;
  QVector<quint32> directSources;
  QVector<quint32> computedSources;
  QVector<quint32> pointerSources;
};

/* Reads both descriptions of the translation out of one pass over the shards.
 * `wantedSources` bounds the instruction map to addresses Ghidra cites: a fully
 * covered cartridge emits hundreds of megabytes of C++. Pass nullptr to read
 * the extents alone, which is what the pass has before Ghidra has run on it and
 * so has nothing to cite yet. */
bool readGbaTranslation(const QString& generatedDir,
                        const QSet<quint32>* wantedSources,
                        QList<GbaTranslatedSpan>& spans,
                        GbaInstructionMap& instructions,
                        QString& error);

/* The extents as SeedGbaEntry.java reads them: one translated function per
 * line, `0xSTART<TAB>0xEND_EXCLUSIVE<TAB>arm|thumb`. This is what confines
 * Ghidra to code the recompiler proved is code. */
QString gbaExtentsTable(const QList<GbaTranslatedSpan>& spans);

/* The span covering `address`, or nullptr. */
const GbaTranslatedSpan* gbaSpanContaining(const QList<GbaTranslatedSpan>& spans,
                                           quint32 address);

/* The number of distinct bytes the spans cover, counting an overlap once. */
quint32 gbaTranslatedBytes(const QList<GbaTranslatedSpan>& spans);

/* The destination of a direct branch at `source`, or false when gba_recompile
 * did not resolve one there — including for an orphan `bl.lo`, whose target is
 * unknowable from the halfword alone. */
bool gbaDirectBranchTarget(const GbaInstructionMap& instructions, quint32 source,
                           const QByteArray& rom, quint32 romBase, quint32& target);

/* True when the instruction's destination is register-borne (a BX or a PC
 * write): control provably leaves and gba_recompile cannot say where. */
bool gbaTransfersThroughRegister(const QString& disassembly);

/* Is this candidate backed by a control transfer gba_recompile itself decoded
 * that can actually reach it? Fills `rule`/`evidence` on admission, setting
 * `modeCrossed` when the evidence sits in the other instruction set; on
 * rejection `reason` is set only when the candidate was contradicted rather
 * than merely unsupported. */
bool admitGbaSeed(const GbaSeedCandidate& candidate,
                  const QList<GbaTranslatedSpan>& spans,
                  const GbaInstructionMap& instructions,
                  const QByteArray& rom, quint32 romBase,
                  QString& rule, quint32& evidence, bool& modeCrossed,
                  QString& reason);

} // namespace psxstudio
