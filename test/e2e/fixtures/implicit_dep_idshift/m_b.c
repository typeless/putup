// Removable unit with no header. Its command is created before m_a's, so removing
// it shifts m_a's command id downward — exercising the id-keyed carry-forward path.
int unused_b(void) { return 0; }
