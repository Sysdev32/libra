#include <drivers/hvfs.h>
#include <drivers/fb.h>
void balright() {
    printk(LOG_ERROR, "balright\n");
    for(;;);
}
void tests() {
    hvfs_init();
    hvfs_create("/RNKL/Tests/balright"); // Note: missing parent "/RNKL/Tests" is auto-created!
    hvfs_set_type("/RNKL/Tests/balright", HVFS_TYPE_FUNCTION);
    
    // 1. Declare a local variable holding the function pointer
    hvfs_func_t fn = balright;
    
    // 2. Pass the ADDRESS of fn, and use sizeof(hvfs_func_t) instead of hardcoded 8
    hvfs_set("/RNKL/Tests/balright", &fn, sizeof(hvfs_func_t));
    
    // 3. Invoke it
    hvfs_call("/RNKL/Tests/balright", NULL);
    
    for(;;);
}