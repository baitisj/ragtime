# Expected pictures for the C++ decoder

`tiny.png` and `tiny-663.png` from the folder above, decoded by the Python
reference (`tools/codecs/rcc/rcc_read.py`), as binary PPM so the C++ test
(`tests/CoolChicTest.cpp`) can read them without a PNG library. The test
allows a level of difference here and there, since the synthesis runs in
floating point.
