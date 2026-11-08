/* Development-only test launcher. Link ASan before loading the ctypes module;
 * the hardened Homebrew Python executable strips DYLD_INSERT_LIBRARIES. */
#include <Python.h>
int main(int argc,char **argv){argv[0]="/opt/homebrew/bin/python3.12";return Py_BytesMain(argc,argv);}
