Steps to perform to get Pintos up and running.
=============================================
0. I am assuming you have Linux running in Virtual Box or natively
1. Make sure your linux is up-to-date
     sudo apt-get update
   
2. Install the C language support (headers and libraries)
     sudo apt-get install clang
   
3. Install x86 Emulator (Qemu) and set it up
     sudo apt-get install qemu
     cd /usr/bin
     sudo ln -s qemu-system-i386 qemu
4. I am going to assume you untar-ed the starter code given to
   some folder (say): /home/<user_name>/EE461S like so:
     cd
     cd EE461S
     tar xvzf ~/Downloads/Pintos-P2.tgz
   You will have a folder now called Pintos-P2
   
5. Modify the file: Pintos-P2/utils/pintos
   Line number 260:
   	my $name =  "/home/ramesh/EE461S/Pintos-P2/userprog/build/kernel.bin";
	
   must have the path to your project2
   
5. Modify the file: Pintos-P2/utils/Pintos.pm
   Line number 363:
        $name = find_file ("/home/ramesh/EE461S/Pintos-P2/userprog/build/loader.bin") if !defined $name;
	
   must have the path to your project2
   
6. Modify the file: Pintos-P2/utils/pintos-gdb
   Line number 5
       GDBMACROS=/home/ramesh/EE461S/Pintos-P2/misc/gdb-macros

   must have the path to your project2


7. Add the utils directory to your PATH
     export PATH=/home/ramesh/EE461S/Pintos-P2/utils:$PATH

8. Build pintos userprogs
     cd userprog
     make
   This will make  your code and build a directory called build

9. This project needs a filesystem, so create it and format it
     cd build
     pintos-mkdisk filesys.dsk --filesys-size=2
     pintos -f -q

10. Now you are ready to test. You can run all 76 test cases like this:
     cd build
     make check

Note steps 8-10 will be repeated as part of yopur development process,
with a 'make clean' thrown in to recompile. make clean will delete the
build directory so you will need to redo step 9 every time you run 'make clean'.


Running Pintos for one test case at a time to Test and Debug
============================================================
To just run one testcase, simply cut-and-pase the command from make check.

For example this is what the output from make check for the very first testcase looks like:
     pintos -v -k -T 60 --qemu  --filesys-size=2 -p tests/userprog/args-none -a args-none -- -q  -f run args-none < /dev/null 2> tests/userprog/args-none.errors > tests/userprog/args-none.output
     perl -I../.. ../../tests/userprog/args-none.ck tests/userprog/args-none tests/userprog/args-none.result
     FAIL tests/userprog/args-none
     Run didn't produce any output

Pintos is launched for the specific testcase (args-none here) and the output produced is stored in a file (tests/userprog/args-none.output here). A perl script is run to compare the output produced with the expected output. The script says FAIL or pass depending on a mismatch or match.

To Debug you will need to launch pintos using gdb (Look at video on Canvas).

   
