#ifndef PSXRECOMP_GTE_TRACE_POLICY_H
#define PSXRECOMP_GTE_TRACE_POLICY_H

/* Raw GTE projection/INTPL capture copies full matrices and vertices for every
 * command. It is an investigation tool, so production enables it only through
 * an explicit environment or TCP request. */
static inline int gte_trace_requested(const char* value)
{
    return value != 0 && value[0] == '1';
}

#endif /* PSXRECOMP_GTE_TRACE_POLICY_H */
