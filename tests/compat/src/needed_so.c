#include "t.h"
extern int aether_test_value(void);
int main(void) { CHECK(aether_test_value() == 4242); DONE("needed_so"); }
