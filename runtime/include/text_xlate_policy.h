#ifndef PSXRECOMP_TEXT_XLATE_POLICY_H
#define PSXRECOMP_TEXT_XLATE_POLICY_H

/* String inventory is an authoring operation, not a production runtime
 * feature. It scans four guest argument registers at every dispatch and can
 * walk/hash up to 512 bytes per candidate. Require an explicit environment
 * opt-in so games without translation work pay only the inactive hook check. */
static inline int text_xlate_capture_requested(const char* value)
{
    return value != 0 && value[0] == '1';
}

#endif /* PSXRECOMP_TEXT_XLATE_POLICY_H */
