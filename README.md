### calibrel: More Accurate Camera Calibration with Imperfect Planar Target

**_Deprecated. This implementation has been integrated into OpenCV 4.0.
See [opencv/opencv/#12772](https://github.com/opencv/opencv/pull/12772)
for details. The C++ interface in OpenCV is `calibrateCameraRO()`_**

This calibration code is based on the paper:
K. H. Strobl and G. Hirzinger. "[More Accurate Pinhole Camera
Calibration with Imperfect Planar
Target](https://www.robotic.dlr.de/fileadmin/robotic/stroblk/publications/strobl_2011iccv.pdf)".
In Proceedings of the IEEE International Conference on Computer Vision
(ICCV 2011), 1st IEEE Workshop on Challenges and Opportunities in Robot
Perception, Barcelona, Spain, pp.  1068-1075, November 2011.

This implementation was validated against [DLR CalDe and DLR
CalLab](http://www.robotic.dlr.de/callab/), a remarkable camera
calibration toolbox, which already includes this method.

The code is largely copied from OpenCV's implementation.

#### How to build

```
$ mkdir build
$ cd build
$ cmake ..
$ make
```

#### Run test program

```
test_calibrel --mode=0 default.xml
```

Where `mode` has one of the following three values:

* **0** Test with this method.
* **1** Test with OpenCV's calibration method.
* **2** Test with the hybrid method, i.e., OpenCV's calibration method
  followed by this method.

See [calibrel_testdata](https://github.com/xoox/calibrel_testdata)
for examples of XML setting files.
