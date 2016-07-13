#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <inttypes.h>
#include <signal.h>

#include <libvmi/libvmi.h>
#include <libvmi/events.h>

static int interrupted = 0;

addr_t register_chrdev_addr;
addr_t mod_sysfs_setup_addr;

vmi_event_t syscall_sysenter_event;

vmi_event_t register_chrdev_single_event;
vmi_event_t mod_sysfs_setup_single_event;

uint32_t register_chrdev_data;
uint32_t mod_sysfs_setup_data;

vmi_pid_t pid = -1;
char *modname = NULL;

event_response_t register_chrdev_single_step_cb(vmi_instance_t vmi, vmi_event_t *event) {

    syscall_sysenter_event.interrupt_event.reinject = 1;
    if (set_breakpoint(vmi, register_chrdev_addr, 0) < 0) {
        fprintf(stderr, "Could not set break points\n");
        exit(1);
    }
    
    vmi_clear_event(vmi, &register_chrdev_single_event, NULL);
    return 0;
}

event_response_t mod_sysfs_setup_single_step_cb(vmi_instance_t vmi, vmi_event_t *event) {

    syscall_sysenter_event.interrupt_event.reinject = 1;
    if (set_breakpoint(vmi, mod_sysfs_setup_addr, 0) < 0) {
        fprintf(stderr, "Could not set break points\n");
        exit(1);
    }
    
    vmi_clear_event(vmi, &mod_sysfs_setup_single_event, NULL);
    return 0;
}


event_response_t syscall_sysenter_cb(vmi_instance_t vmi, vmi_event_t *event){
    reg_t rdi, rcx, cr3;
    vmi_get_vcpureg(vmi, &rdi, RDI, event->vcpu_id);
    vmi_get_vcpureg(vmi, &rcx, RCX, event->vcpu_id);
    vmi_get_vcpureg(vmi, &cr3, CR3, event->vcpu_id);

    pid = vmi_dtb_to_pid(vmi, cr3);
    if (event->interrupt_event.gla == register_chrdev_addr) {
        char *argname = NULL;
        argname = vmi_read_str_va(vmi, rcx, 0);
        printf("Process [%d] register a Character Device: %s\n", pid, argname);

        event->interrupt_event.reinject = 0;
        if (VMI_FAILURE == vmi_write_32_va(vmi, register_chrdev_addr, 0, &register_chrdev_data)) {
            fprintf(stderr, "failed to write memory.\n");
            exit(1);
        }
        vmi_register_event(vmi, &register_chrdev_single_event);

    } else if (event->interrupt_event.gla == mod_sysfs_setup_addr) {
        modname = vmi_read_str_va(vmi, (addr_t)rdi+24, 0);
        printf("Process [%d] inserts a kernel module: %s\n", pid, modname);

        event->interrupt_event.reinject = 0;
        if (VMI_FAILURE == vmi_write_32_va(vmi, mod_sysfs_setup_addr, 0, &mod_sysfs_setup_data)) {
            fprintf(stderr, "failed to write memory.\n");
            exit(1);
        }
        vmi_register_event(vmi, &mod_sysfs_setup_single_event);

    }

    return 0;
}

int set_breakpoint(vmi_instance_t vmi, addr_t addr, pid_t pid) {

    uint32_t data;
    if (VMI_FAILURE == vmi_read_32_va(vmi, addr, pid, &data)) {
        printf("failed to read memory.\n");
        return -1;
    }
    data = (data & 0xFFFFFF00) | 0xCC;
    if (VMI_FAILURE == vmi_write_32_va(vmi, addr, pid, &data)) {
        printf("failed to write memory.\n");
        return -1;
    }
    return 0;
}

static void close_handler(int sig){
    interrupted = sig;
}

int main (int argc, char **argv) {

    if(argc < 2){
        fprintf(stderr, "Usage: events_example <name of VM> <PID of process to track {optional}>\n");
        exit(1);
    }

    char *name = NULL;
    name = argv[1];


    struct sigaction act;
    act.sa_handler = close_handler;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction(SIGHUP,  &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT,  &act, NULL);
    sigaction(SIGALRM, &act, NULL);

    vmi_instance_t vmi = NULL;
    if (vmi_init(&vmi, VMI_XEN | VMI_INIT_COMPLETE | VMI_INIT_EVENTS, name) == VMI_FAILURE){
        printf("Failed to init LibVMI library.\n");
        if (vmi != NULL )
            vmi_destroy(vmi);
        return 1;
    }
    else
        printf("LibVMI init succeeded!\n");


    register_chrdev_addr = vmi_translate_ksym2v(vmi, "__register_chrdev");
    printf("__register_chrdev address is 0x%x\n", (unsigned int)register_chrdev_addr);

    mod_sysfs_setup_addr = vmi_translate_ksym2v(vmi, "mod_sysfs_setup");
    printf("mod_sysfs_setup address is 0x%x\n", (unsigned int)mod_sysfs_setup_addr);

    memset(&syscall_sysenter_event, 0, sizeof(vmi_event_t));
    syscall_sysenter_event.type = VMI_EVENT_INTERRUPT;
    syscall_sysenter_event.interrupt_event.intr = INT3;
    syscall_sysenter_event.callback = syscall_sysenter_cb;

    memset(&register_chrdev_single_event, 0, sizeof(vmi_event_t));
    register_chrdev_single_event.type = VMI_EVENT_SINGLESTEP;
    register_chrdev_single_event.callback = register_chrdev_single_step_cb;
    register_chrdev_single_event.ss_event.enable = 1;
    SET_VCPU_SINGLESTEP(register_chrdev_single_event.ss_event, 0);

    memset(&mod_sysfs_setup_single_event, 0, sizeof(vmi_event_t));
    mod_sysfs_setup_single_event.type = VMI_EVENT_SINGLESTEP;
    mod_sysfs_setup_single_event.callback = mod_sysfs_setup_single_step_cb;
    mod_sysfs_setup_single_event.ss_event.enable = 1;
    SET_VCPU_SINGLESTEP(mod_sysfs_setup_single_event.ss_event, 0);

    if (VMI_FAILURE == vmi_read_32_va(vmi, register_chrdev_addr, 0, &register_chrdev_data)) {
        printf("failed to read memory.\n");
        vmi_destroy(vmi);
        return -1;
    }

    if (VMI_FAILURE == vmi_read_32_va(vmi, mod_sysfs_setup_addr, 0, &mod_sysfs_setup_data)) {
        printf("failed to read memory.\n");
        vmi_destroy(vmi);
        return -1;
    }

    if(vmi_register_event(vmi, &syscall_sysenter_event) == VMI_FAILURE) {
        fprintf(stderr, "Could not install sysenter syscall handler.\n");
        goto leave;
    }

    if (set_breakpoint(vmi, register_chrdev_addr, 0) < 0) {
        fprintf(stderr, "Could not set break points\n");
        goto leave;
    }

    if (set_breakpoint(vmi, mod_sysfs_setup_addr, 0) < 0) {
        fprintf(stderr, "Could not set break points\n");
        goto leave;
    }

    status_t status;
    while(!interrupted){
        status = vmi_events_listen(vmi, 1000);
        if (status != VMI_SUCCESS) {
            printf("Error waiting for events, quitting...\n");
            interrupted = -1;
        }
    }
    printf("Finished with test.\n");

leave:
    if (VMI_FAILURE == vmi_write_32_va(vmi, register_chrdev_addr, 0, &register_chrdev_data)) {
        fprintf(stderr, "failed to write memory.\n");
        exit(1);
    }

    if (VMI_FAILURE == vmi_write_32_va(vmi, mod_sysfs_setup_addr, 0, &mod_sysfs_setup_data)) {
        fprintf(stderr, "failed to write memory.\n");
        exit(1);
    }

    vmi_destroy(vmi);

    return 0;
}
