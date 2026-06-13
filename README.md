# Libra
Hello my fellow developers. I've started this project as a hobby kernel. It's a sequel to my previous kernels (unreleased).
It aims to be an alternative to linux that is quite a bit more stable, but this kernel is still quite experimental, and we are super far away from this goal. So, it's a pleasure if anyone would like to help with this project.

## Getting Started
### Project Instructions
I've taken inspiration from other kernels and see that this section for devs is mostly unclear.
The actual low-level kernel assembly code isn't there. This kernel boots directly to C using Limine.
If you are unfamiliar with Limine, I'd recommend for you to check it out before developing here.
This is a higher-half kernel. Limine has mapped the pages already, and I'd prefer that we edit and shall not **REPLACE** the memory map.
### Creating a branch
Before submitting your edits. Please create a branch using this format: user_name-YY-MM-DD-v#.#.#-summary_of_your_edit.
And then create a pull request. Within a small period of time it might be accepted by me or not.

### Small Notes

The user Zirconium is also me, just another account, my Git was misconfigured at the time.