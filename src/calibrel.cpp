/*
    IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.

    By downloading, copying, installing or using the software you agree
    to this license. If you do not agree to this license, do not
    download, install, copy or use the software.

                             License Agreement
                  For Open Source Computer Vision Library

    Copyright (C) 2000-2008, Intel Corporation, all rights reserved.
    Copyright (C) 2009, Willow Garage Inc., all rights reserved.
    Copyright (C) 2018, Wenfeng CAI, all rights reserved.
    Third party copyrights are property of their respective owners.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

      * Redistribution's of source code must retain the above copyright
        notice, this list of conditions and the following disclaimer.

      * Redistribution's in binary form must reproduce the above
        copyright notice, this list of conditions and the following
        disclaimer in the documentation and/or other materials provided
        with the distribution.

      * The name of the copyright holders may not be used to endorse or
        promote products derived from this software without specific
        prior written permission.

    This software is provided by the copyright holders and contributors
    "as is" and any express or implied warranties, including, but not
    limited to, the implied warranties of merchantability and fitness
    for a particular purpose are disclaimed.  In no event shall the
    Intel Corporation or contributors be liable for any direct,
    indirect, incidental, special, exemplary, or consequential damages
    (including, but not limited to, procurement of substitute goods or
    services; loss of use, data, or profits; or business interruption)
    however caused and on any theory of liability, whether in contract,
    strict liability, or tort (including negligence or otherwise)
    arising in any way out of the use of this software, even if advised
    of the possibility of such damage.
 */

#include "calibrel/calibrel.hpp"

/*
    This is (in a large extent) based on the paper:
    Klaus H. Strobl and Gerd Hirzinger. "More Accurate Pinhole Camera
    Calibration with Imperfect Planar Target".
    2011 IEEE International Conference on Computer Vision Workshops.
 */

namespace calrel {

using namespace cv;

static const char* cvDistCoeffErr = "Distortion coefficients must be 1x4, "
                                    "4x1, 1x5, 5x1, 1x8, 8x1, 1x12, 12x1, "
                                    "1x14 or 14x1 floating-point vector";

static void projectPoints2(const CvMat* objectPoints, const CvMat* r_vec,
    const CvMat* t_vec, const CvMat* A, const CvMat* distCoeffs,
    CvMat* imagePoints, CvMat* dpdr = NULL, CvMat* dpdt = NULL,
    CvMat* dpdf = NULL, CvMat* dpdc = NULL, CvMat* dpdk = NULL,
    CvMat* dpdo = NULL, double aspectRatio = 0)
{
    Ptr<CvMat> matM, _m;
    Ptr<CvMat> _dpdr, _dpdt, _dpdc, _dpdf, _dpdk, _dpdo;

    int i, j, count;
    int calc_derivatives;
    const CvPoint3D64f* M;
    CvPoint2D64f* m;
    double r[3], R[9], dRdr[27], t[3], a[9],
        k[14] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, fx, fy, cx, cy;
    Matx33d matTilt = Matx33d::eye();
    Matx33d dMatTiltdTauX(0, 0, 0, 0, 0, 0, 0, -1, 0);
    Matx33d dMatTiltdTauY(0, 0, 0, 0, 0, 0, 1, 0, 0);
    CvMat _r, _t, _a = cvMat(3, 3, CV_64F, a), _k;
    CvMat matR = cvMat(3, 3, CV_64F, R), _dRdr = cvMat(3, 9, CV_64F, dRdr);
    double *dpdr_p = 0, *dpdt_p = 0, *dpdk_p = 0, *dpdf_p = 0, *dpdc_p = 0,
           *dpdo_p = 0;
    int dpdr_step = 0, dpdt_step = 0, dpdk_step = 0, dpdf_step = 0,
        dpdc_step = 0, dpdo_step = 0;
    bool fixedAspectRatio = aspectRatio > FLT_EPSILON;

    if (!CV_IS_MAT(objectPoints) || !CV_IS_MAT(r_vec) || !CV_IS_MAT(t_vec)
        || !CV_IS_MAT(A) ||
        /*!CV_IS_MAT(distCoeffs) ||*/ !CV_IS_MAT(imagePoints))
        CV_Error(
            CV_StsBadArg, "One of required arguments is not a valid matrix");

    int total = objectPoints->rows * objectPoints->cols
        * CV_MAT_CN(objectPoints->type);
    if (total % 3 != 0) {
        // we have stopped support of homogeneous coordinates because it cause
        // ambiguity in interpretation of the input data
        CV_Error(CV_StsBadArg, "Homogeneous coordinates are not supported");
    }
    count = total / 3;

    if (CV_IS_CONT_MAT(objectPoints->type)
        && (CV_MAT_DEPTH(objectPoints->type) == CV_32F
               || CV_MAT_DEPTH(objectPoints->type) == CV_64F)
        && ((objectPoints->rows == 1 && CV_MAT_CN(objectPoints->type) == 3)
               || (objectPoints->rows == count
                      && CV_MAT_CN(objectPoints->type) * objectPoints->cols
                          == 3)
               || (objectPoints->rows == 3
                      && CV_MAT_CN(objectPoints->type) == 1
                      && objectPoints->cols == count))) {
        matM.reset(cvCreateMat(objectPoints->rows, objectPoints->cols,
            CV_MAKETYPE(CV_64F, CV_MAT_CN(objectPoints->type))));
        cvConvert(objectPoints, matM);
    } else {
        //        matM = cvCreateMat( 1, count, CV_64FC3 );
        //        cvConvertPointsHomogeneous( objectPoints, matM );
        CV_Error(CV_StsBadArg, "Homogeneous coordinates are not supported");
    }

    if (CV_IS_CONT_MAT(imagePoints->type)
        && (CV_MAT_DEPTH(imagePoints->type) == CV_32F
               || CV_MAT_DEPTH(imagePoints->type) == CV_64F)
        && ((imagePoints->rows == 1 && CV_MAT_CN(imagePoints->type) == 2)
               || (imagePoints->rows == count
                      && CV_MAT_CN(imagePoints->type) * imagePoints->cols
                          == 2)
               || (imagePoints->rows == 2 && CV_MAT_CN(imagePoints->type) == 1
                      && imagePoints->cols == count))) {
        _m.reset(cvCreateMat(imagePoints->rows, imagePoints->cols,
            CV_MAKETYPE(CV_64F, CV_MAT_CN(imagePoints->type))));
        cvConvert(imagePoints, _m);
    } else {
        //        _m = cvCreateMat( 1, count, CV_64FC2 );
        CV_Error(CV_StsBadArg, "Homogeneous coordinates are not supported");
    }

    M = (CvPoint3D64f*)matM->data.db;
    m = (CvPoint2D64f*)_m->data.db;

    if ((CV_MAT_DEPTH(r_vec->type) != CV_64F
            && CV_MAT_DEPTH(r_vec->type) != CV_32F)
        || (((r_vec->rows != 1 && r_vec->cols != 1)
                || r_vec->rows * r_vec->cols * CV_MAT_CN(r_vec->type) != 3)
               && ((r_vec->rows != 3 && r_vec->cols != 3)
                      || CV_MAT_CN(r_vec->type) != 1)))
        CV_Error(CV_StsBadArg,
            "Rotation must be represented by 1x3 or 3x1 "
            "floating-point rotation vector, or 3x3 rotation matrix");

    if (r_vec->rows == 3 && r_vec->cols == 3) {
        _r = cvMat(3, 1, CV_64FC1, r);
        cvRodrigues2(r_vec, &_r);
        cvRodrigues2(&_r, &matR, &_dRdr);
        cvCopy(r_vec, &matR);
    } else {
        _r = cvMat(r_vec->rows, r_vec->cols,
            CV_MAKETYPE(CV_64F, CV_MAT_CN(r_vec->type)), r);
        cvConvert(r_vec, &_r);
        cvRodrigues2(&_r, &matR, &_dRdr);
    }

    if ((CV_MAT_DEPTH(t_vec->type) != CV_64F
            && CV_MAT_DEPTH(t_vec->type) != CV_32F)
        || (t_vec->rows != 1 && t_vec->cols != 1)
        || t_vec->rows * t_vec->cols * CV_MAT_CN(t_vec->type) != 3)
        CV_Error(CV_StsBadArg,
            "Translation vector must be 1x3 or 3x1 floating-point vector");

    _t = cvMat(t_vec->rows, t_vec->cols,
        CV_MAKETYPE(CV_64F, CV_MAT_CN(t_vec->type)), t);
    cvConvert(t_vec, &_t);

    if ((CV_MAT_TYPE(A->type) != CV_64FC1 && CV_MAT_TYPE(A->type) != CV_32FC1)
        || A->rows != 3 || A->cols != 3)
        CV_Error(CV_StsBadArg,
            "Instrinsic parameters must be 3x3 floating-point matrix");

    cvConvert(A, &_a);
    fx = a[0];
    fy = a[4];
    cx = a[2];
    cy = a[5];

    if (fixedAspectRatio)
        fx = fy * aspectRatio;

    if (distCoeffs) {
        if (!CV_IS_MAT(distCoeffs)
            || (CV_MAT_DEPTH(distCoeffs->type) != CV_64F
                   && CV_MAT_DEPTH(distCoeffs->type) != CV_32F)
            || (distCoeffs->rows != 1 && distCoeffs->cols != 1)
            || (distCoeffs->rows * distCoeffs->cols
                           * CV_MAT_CN(distCoeffs->type)
                       != 4
                   && distCoeffs->rows * distCoeffs->cols
                           * CV_MAT_CN(distCoeffs->type)
                       != 5
                   && distCoeffs->rows * distCoeffs->cols
                           * CV_MAT_CN(distCoeffs->type)
                       != 8
                   && distCoeffs->rows * distCoeffs->cols
                           * CV_MAT_CN(distCoeffs->type)
                       != 12
                   && distCoeffs->rows * distCoeffs->cols
                           * CV_MAT_CN(distCoeffs->type)
                       != 14))
            CV_Error(CV_StsBadArg, cvDistCoeffErr);

        _k = cvMat(distCoeffs->rows, distCoeffs->cols,
            CV_MAKETYPE(CV_64F, CV_MAT_CN(distCoeffs->type)), k);
        cvConvert(distCoeffs, &_k);
        if (k[12] != 0 || k[13] != 0) {
            detail::computeTiltProjectionMatrix(
                k[12], k[13], &matTilt, &dMatTiltdTauX, &dMatTiltdTauY);
        }
    }

    if (dpdr) {
        if (!CV_IS_MAT(dpdr) || (CV_MAT_TYPE(dpdr->type) != CV_32FC1
                                    && CV_MAT_TYPE(dpdr->type) != CV_64FC1)
            || dpdr->rows != count * 2 || dpdr->cols != 3)
            CV_Error(
                CV_StsBadArg, "dp/drot must be 2Nx3 floating-point matrix");

        if (CV_MAT_TYPE(dpdr->type) == CV_64FC1) {
            _dpdr.reset(cvCloneMat(dpdr));
        } else
            _dpdr.reset(cvCreateMat(2 * count, 3, CV_64FC1));
        dpdr_p = _dpdr->data.db;
        dpdr_step = _dpdr->step / sizeof(dpdr_p[0]);
    }

    if (dpdt) {
        if (!CV_IS_MAT(dpdt) || (CV_MAT_TYPE(dpdt->type) != CV_32FC1
                                    && CV_MAT_TYPE(dpdt->type) != CV_64FC1)
            || dpdt->rows != count * 2 || dpdt->cols != 3)
            CV_Error(
                CV_StsBadArg, "dp/dT must be 2Nx3 floating-point matrix");

        if (CV_MAT_TYPE(dpdt->type) == CV_64FC1) {
            _dpdt.reset(cvCloneMat(dpdt));
        } else
            _dpdt.reset(cvCreateMat(2 * count, 3, CV_64FC1));
        dpdt_p = _dpdt->data.db;
        dpdt_step = _dpdt->step / sizeof(dpdt_p[0]);
    }

    if (dpdf) {
        if (!CV_IS_MAT(dpdf) || (CV_MAT_TYPE(dpdf->type) != CV_32FC1
                                    && CV_MAT_TYPE(dpdf->type) != CV_64FC1)
            || dpdf->rows != count * 2 || dpdf->cols != 2)
            CV_Error(
                CV_StsBadArg, "dp/df must be 2Nx2 floating-point matrix");

        if (CV_MAT_TYPE(dpdf->type) == CV_64FC1) {
            _dpdf.reset(cvCloneMat(dpdf));
        } else
            _dpdf.reset(cvCreateMat(2 * count, 2, CV_64FC1));
        dpdf_p = _dpdf->data.db;
        dpdf_step = _dpdf->step / sizeof(dpdf_p[0]);
    }

    if (dpdc) {
        if (!CV_IS_MAT(dpdc) || (CV_MAT_TYPE(dpdc->type) != CV_32FC1
                                    && CV_MAT_TYPE(dpdc->type) != CV_64FC1)
            || dpdc->rows != count * 2 || dpdc->cols != 2)
            CV_Error(
                CV_StsBadArg, "dp/dc must be 2Nx2 floating-point matrix");

        if (CV_MAT_TYPE(dpdc->type) == CV_64FC1) {
            _dpdc.reset(cvCloneMat(dpdc));
        } else
            _dpdc.reset(cvCreateMat(2 * count, 2, CV_64FC1));
        dpdc_p = _dpdc->data.db;
        dpdc_step = _dpdc->step / sizeof(dpdc_p[0]);
    }

    if (dpdk) {
        if (!CV_IS_MAT(dpdk) || (CV_MAT_TYPE(dpdk->type) != CV_32FC1
                                    && CV_MAT_TYPE(dpdk->type) != CV_64FC1)
            || dpdk->rows != count * 2
            || (dpdk->cols != 14 && dpdk->cols != 12 && dpdk->cols != 8
                   && dpdk->cols != 5 && dpdk->cols != 4 && dpdk->cols != 2))
            CV_Error(CV_StsBadArg, "dp/df must be 2Nx14, 2Nx12, 2Nx8, 2Nx5, "
                                   "2Nx4 or 2Nx2 floating-point matrix");

        if (!distCoeffs)
            CV_Error(CV_StsNullPtr, "distCoeffs is NULL while dpdk is not");

        if (CV_MAT_TYPE(dpdk->type) == CV_64FC1) {
            _dpdk.reset(cvCloneMat(dpdk));
        } else
            _dpdk.reset(cvCreateMat(dpdk->rows, dpdk->cols, CV_64FC1));
        dpdk_p = _dpdk->data.db;
        dpdk_step = _dpdk->step / sizeof(dpdk_p[0]);
    }

    if (dpdo) {
        if (!CV_IS_MAT(dpdo) || (CV_MAT_TYPE(dpdo->type) != CV_32FC1
                                    && CV_MAT_TYPE(dpdo->type) != CV_64FC1)
            || dpdo->rows != count * 2 || dpdo->cols != count * 3)
            CV_Error(
                CV_StsBadArg, "dp/do must be 2Nx3N floating-point matrix");

        if (CV_MAT_TYPE(dpdo->type) == CV_64FC1) {
            _dpdo.reset(cvCloneMat(dpdo));
        } else
            _dpdo.reset(cvCreateMat(2 * count, 3 * count, CV_64FC1));
        dpdo_p = _dpdo->data.db;
        dpdo_step = _dpdo->step / sizeof(dpdo_p[0]);
    }

    calc_derivatives = dpdr || dpdt || dpdf || dpdc || dpdk || dpdo;

    for (i = 0; i < count; i++) {
        double X = M[i].x, Y = M[i].y, Z = M[i].z;
        double x = R[0] * X + R[1] * Y + R[2] * Z + t[0];
        double y = R[3] * X + R[4] * Y + R[5] * Z + t[1];
        double z = R[6] * X + R[7] * Y + R[8] * Z + t[2];
        double r2, r4, r6, a1, a2, a3, cdist, icdist2;
        double xd, yd, xd0, yd0, invProj;
        Vec3d vecTilt;
        Vec3d dVecTilt;
        Matx22d dMatTilt;
        Vec2d dXdYd;

        double z0 = z;
        z = z ? 1. / z : 1;
        x *= z;
        y *= z;

        r2 = x * x + y * y;
        r4 = r2 * r2;
        r6 = r4 * r2;
        a1 = 2 * x * y;
        a2 = r2 + 2 * x * x;
        a3 = r2 + 2 * y * y;
        cdist = 1 + k[0] * r2 + k[1] * r4 + k[4] * r6;
        icdist2 = 1. / (1 + k[5] * r2 + k[6] * r4 + k[7] * r6);
        xd0 = x * cdist * icdist2 + k[2] * a1 + k[3] * a2 + k[8] * r2
            + k[9] * r4;
        yd0 = y * cdist * icdist2 + k[2] * a3 + k[3] * a1 + k[10] * r2
            + k[11] * r4;

        // additional distortion by projecting onto a tilt plane
        vecTilt = matTilt * Vec3d(xd0, yd0, 1);
        invProj = vecTilt(2) ? 1. / vecTilt(2) : 1;
        xd = invProj * vecTilt(0);
        yd = invProj * vecTilt(1);

        m[i].x = xd * fx + cx;
        m[i].y = yd * fy + cy;

        if (calc_derivatives) {
            if (dpdc_p) {
                dpdc_p[0] = 1;
                dpdc_p[1] = 0; // dp_xdc_x; dp_xdc_y
                dpdc_p[dpdc_step] = 0;
                dpdc_p[dpdc_step + 1] = 1;
                dpdc_p += dpdc_step * 2;
            }

            if (dpdf_p) {
                if (fixedAspectRatio) {
                    dpdf_p[0] = 0;
                    dpdf_p[1] = xd * aspectRatio; // dp_xdf_x; dp_xdf_y
                    dpdf_p[dpdf_step] = 0;
                    dpdf_p[dpdf_step + 1] = yd;
                } else {
                    dpdf_p[0] = xd;
                    dpdf_p[1] = 0;
                    dpdf_p[dpdf_step] = 0;
                    dpdf_p[dpdf_step + 1] = yd;
                }
                dpdf_p += dpdf_step * 2;
            }
            for (int row = 0; row < 2; ++row)
                for (int col = 0; col < 2; ++col)
                    dMatTilt(row, col) = matTilt(row, col) * vecTilt(2)
                        - matTilt(2, col) * vecTilt(row);
            double invProjSquare = (invProj * invProj);
            dMatTilt *= invProjSquare;
            if (dpdk_p) {
                dXdYd = dMatTilt * Vec2d(x * icdist2 * r2, y * icdist2 * r2);
                dpdk_p[0] = fx * dXdYd(0);
                dpdk_p[dpdk_step] = fy * dXdYd(1);
                dXdYd = dMatTilt * Vec2d(x * icdist2 * r4, y * icdist2 * r4);
                dpdk_p[1] = fx * dXdYd(0);
                dpdk_p[dpdk_step + 1] = fy * dXdYd(1);
                if (_dpdk->cols > 2) {
                    dXdYd = dMatTilt * Vec2d(a1, a3);
                    dpdk_p[2] = fx * dXdYd(0);
                    dpdk_p[dpdk_step + 2] = fy * dXdYd(1);
                    dXdYd = dMatTilt * Vec2d(a2, a1);
                    dpdk_p[3] = fx * dXdYd(0);
                    dpdk_p[dpdk_step + 3] = fy * dXdYd(1);
                    if (_dpdk->cols > 4) {
                        dXdYd = dMatTilt
                            * Vec2d(x * icdist2 * r6, y * icdist2 * r6);
                        dpdk_p[4] = fx * dXdYd(0);
                        dpdk_p[dpdk_step + 4] = fy * dXdYd(1);

                        if (_dpdk->cols > 5) {
                            dXdYd = dMatTilt
                                * Vec2d(x * cdist * (-icdist2) * icdist2 * r2,
                                      y * cdist * (-icdist2) * icdist2 * r2);
                            dpdk_p[5] = fx * dXdYd(0);
                            dpdk_p[dpdk_step + 5] = fy * dXdYd(1);
                            dXdYd = dMatTilt
                                * Vec2d(x * cdist * (-icdist2) * icdist2 * r4,
                                      y * cdist * (-icdist2) * icdist2 * r4);
                            dpdk_p[6] = fx * dXdYd(0);
                            dpdk_p[dpdk_step + 6] = fy * dXdYd(1);
                            dXdYd = dMatTilt
                                * Vec2d(x * cdist * (-icdist2) * icdist2 * r6,
                                      y * cdist * (-icdist2) * icdist2 * r6);
                            dpdk_p[7] = fx * dXdYd(0);
                            dpdk_p[dpdk_step + 7] = fy * dXdYd(1);
                            if (_dpdk->cols > 8) {
                                dXdYd = dMatTilt * Vec2d(r2, 0);
                                dpdk_p[8] = fx * dXdYd(0); // s1
                                dpdk_p[dpdk_step + 8] = fy * dXdYd(1); // s1
                                dXdYd = dMatTilt * Vec2d(r4, 0);
                                dpdk_p[9] = fx * dXdYd(0); // s2
                                dpdk_p[dpdk_step + 9] = fy * dXdYd(1); // s2
                                dXdYd = dMatTilt * Vec2d(0, r2);
                                dpdk_p[10] = fx * dXdYd(0); // s3
                                dpdk_p[dpdk_step + 10] = fy * dXdYd(1); // s3
                                dXdYd = dMatTilt * Vec2d(0, r4);
                                dpdk_p[11] = fx * dXdYd(0); // s4
                                dpdk_p[dpdk_step + 11] = fy * dXdYd(1); // s4
                                if (_dpdk->cols > 12) {
                                    dVecTilt
                                        = dMatTiltdTauX * Vec3d(xd0, yd0, 1);
                                    dpdk_p[12] = fx * invProjSquare
                                        * (dVecTilt(0) * vecTilt(2)
                                              - dVecTilt(2) * vecTilt(0));
                                    dpdk_p[dpdk_step + 12] = fy
                                        * invProjSquare
                                        * (dVecTilt(1) * vecTilt(2)
                                              - dVecTilt(2) * vecTilt(1));
                                    dVecTilt
                                        = dMatTiltdTauY * Vec3d(xd0, yd0, 1);
                                    dpdk_p[13] = fx * invProjSquare
                                        * (dVecTilt(0) * vecTilt(2)
                                              - dVecTilt(2) * vecTilt(0));
                                    dpdk_p[dpdk_step + 13] = fy
                                        * invProjSquare
                                        * (dVecTilt(1) * vecTilt(2)
                                              - dVecTilt(2) * vecTilt(1));
                                }
                            }
                        }
                    }
                }
                dpdk_p += dpdk_step * 2;
            }

            if (dpdt_p) {
                double dxdt[] = { z, 0, -x * z }, dydt[] = { 0, z, -y * z };
                for (j = 0; j < 3; j++) {
                    double dr2dt = 2 * x * dxdt[j] + 2 * y * dydt[j];
                    double dcdist_dt = k[0] * dr2dt + 2 * k[1] * r2 * dr2dt
                        + 3 * k[4] * r4 * dr2dt;
                    double dicdist2_dt = -icdist2 * icdist2
                        * (k[5] * dr2dt + 2 * k[6] * r2 * dr2dt
                              + 3 * k[7] * r4 * dr2dt);
                    double da1dt = 2 * (x * dydt[j] + y * dxdt[j]);
                    double dmxdt = (dxdt[j] * cdist * icdist2
                        + x * dcdist_dt * icdist2 + x * cdist * dicdist2_dt
                        + k[2] * da1dt + k[3] * (dr2dt + 4 * x * dxdt[j])
                        + k[8] * dr2dt + 2 * r2 * k[9] * dr2dt);
                    double dmydt = (dydt[j] * cdist * icdist2
                        + y * dcdist_dt * icdist2 + y * cdist * dicdist2_dt
                        + k[2] * (dr2dt + 4 * y * dydt[j]) + k[3] * da1dt
                        + k[10] * dr2dt + 2 * r2 * k[11] * dr2dt);
                    dXdYd = dMatTilt * Vec2d(dmxdt, dmydt);
                    dpdt_p[j] = fx * dXdYd(0);
                    dpdt_p[dpdt_step + j] = fy * dXdYd(1);
                }
                dpdt_p += dpdt_step * 2;
            }

            if (dpdr_p) {
                double dx0dr[] = { X * dRdr[0] + Y * dRdr[1] + Z * dRdr[2],
                    X * dRdr[9] + Y * dRdr[10] + Z * dRdr[11],
                    X * dRdr[18] + Y * dRdr[19] + Z * dRdr[20] };
                double dy0dr[] = { X * dRdr[3] + Y * dRdr[4] + Z * dRdr[5],
                    X * dRdr[12] + Y * dRdr[13] + Z * dRdr[14],
                    X * dRdr[21] + Y * dRdr[22] + Z * dRdr[23] };
                double dz0dr[] = { X * dRdr[6] + Y * dRdr[7] + Z * dRdr[8],
                    X * dRdr[15] + Y * dRdr[16] + Z * dRdr[17],
                    X * dRdr[24] + Y * dRdr[25] + Z * dRdr[26] };
                for (j = 0; j < 3; j++) {
                    double dxdr = z * (dx0dr[j] - x * dz0dr[j]);
                    double dydr = z * (dy0dr[j] - y * dz0dr[j]);
                    double dr2dr = 2 * x * dxdr + 2 * y * dydr;
                    double dcdist_dr
                        = (k[0] + 2 * k[1] * r2 + 3 * k[4] * r4) * dr2dr;
                    double dicdist2_dr = -icdist2 * icdist2
                        * (k[5] + 2 * k[6] * r2 + 3 * k[7] * r4) * dr2dr;
                    double da1dr = 2 * (x * dydr + y * dxdr);
                    double dmxdr = (dxdr * cdist * icdist2
                        + x * dcdist_dr * icdist2 + x * cdist * dicdist2_dr
                        + k[2] * da1dr + k[3] * (dr2dr + 4 * x * dxdr)
                        + (k[8] + 2 * r2 * k[9]) * dr2dr);
                    double dmydr = (dydr * cdist * icdist2
                        + y * dcdist_dr * icdist2 + y * cdist * dicdist2_dr
                        + k[2] * (dr2dr + 4 * y * dydr) + k[3] * da1dr
                        + (k[10] + 2 * r2 * k[11]) * dr2dr);
                    dXdYd = dMatTilt * Vec2d(dmxdr, dmydr);
                    dpdr_p[j] = fx * dXdYd(0);
                    dpdr_p[dpdr_step + j] = fy * dXdYd(1);
                }
                dpdr_p += dpdr_step * 2;
            }

            if (dpdo_p) {
                double dxdo[] = { z * (R[0] - x * z * z0 * R[6]),
                    z * (R[1] - x * z * z0 * R[7]),
                    z * (R[2] - x * z * z0 * R[8]) };
                double dydo[] = { z * (R[3] - y * z * z0 * R[6]),
                    z * (R[4] - y * z * z0 * R[7]),
                    z * (R[5] - y * z * z0 * R[8]) };
                for (j = 0; j < 3; j++) {
                    double dr2do = 2 * x * dxdo[j] + 2 * y * dydo[j];
                    double dr4do = 2 * r2 * dr2do;
                    double dr6do = 3 * r4 * dr2do;
                    double da1do = 2 * y * dxdo[j] + 2 * x * dydo[j];
                    double da2do = dr2do + 4 * x * dxdo[j];
                    double da3do = dr2do + 4 * y * dydo[j];
                    double dcdist_do
                        = k[0] * dr2do + k[1] * dr4do + k[4] * dr6do;
                    double dicdist2_do = -icdist2 * icdist2
                        * (k[5] * dr2do + k[6] * dr4do + k[7] * dr6do);
                    double dxd0_do = cdist * icdist2 * dxdo[j]
                        + x * icdist2 * dcdist_do + x * cdist * dicdist2_do
                        + k[2] * da1do + k[3] * da2do + k[8] * dr2do
                        + k[9] * dr4do;
                    double dyd0_do = cdist * icdist2 * dydo[j]
                        + y * icdist2 * dcdist_do + y * cdist * dicdist2_do
                        + k[2] * da3do + k[3] * da1do + k[10] * dr2do
                        + k[11] * dr4do;
                    dXdYd = dMatTilt * Vec2d(dxd0_do, dyd0_do);
                    dpdo_p[i * 3 + j] = fx * dXdYd(0);
                    dpdo_p[dpdo_step + i * 3 + j] = fy * dXdYd(1);
                }
                dpdo_p += dpdo_step * 2;
            }
        }
    }

    if (_m != imagePoints)
        cvConvert(_m, imagePoints);

    if (_dpdr != dpdr)
        cvConvert(_dpdr, dpdr);

    if (_dpdt != dpdt)
        cvConvert(_dpdt, dpdt);

    if (_dpdf != dpdf)
        cvConvert(_dpdf, dpdf);

    if (_dpdc != dpdc)
        cvConvert(_dpdc, dpdc);

    if (_dpdk != dpdk)
        cvConvert(_dpdk, dpdk);

    if (_dpdo != dpdo)
        cvConvert(_dpdo, dpdo);
}

static void findExtrinsicCameraParams2(const CvMat* objectPoints,
    const CvMat* imagePoints, const CvMat* A, const CvMat* distCoeffs,
    CvMat* rvec, CvMat* tvec, int useExtrinsicGuess = 0)
{
    const int max_iter = 20;
    Ptr<CvMat> matM, _Mxy, _m, _mn, matL;

    int i, count;
    double a[9], ar[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, R[9];
    double MM[9], U[9], V[9], W[3];
    cv::Scalar Mc;
    double param[6];
    CvMat matA = cvMat(3, 3, CV_64F, a);
    CvMat _Ar = cvMat(3, 3, CV_64F, ar);
    CvMat matR = cvMat(3, 3, CV_64F, R);
    CvMat _r = cvMat(3, 1, CV_64F, param);
    CvMat _t = cvMat(3, 1, CV_64F, param + 3);
    CvMat _Mc = cvMat(1, 3, CV_64F, Mc.val);
    CvMat _MM = cvMat(3, 3, CV_64F, MM);
    CvMat matU = cvMat(3, 3, CV_64F, U);
    CvMat matV = cvMat(3, 3, CV_64F, V);
    CvMat matW = cvMat(3, 1, CV_64F, W);
    CvMat _param = cvMat(6, 1, CV_64F, param);
    CvMat _dpdr, _dpdt;

    CV_Assert(CV_IS_MAT(objectPoints) && CV_IS_MAT(imagePoints)
        && CV_IS_MAT(A) && CV_IS_MAT(rvec) && CV_IS_MAT(tvec));

    count = MAX(objectPoints->cols, objectPoints->rows);
    matM.reset(cvCreateMat(1, count, CV_64FC3));
    _m.reset(cvCreateMat(1, count, CV_64FC2));

    cvConvert(objectPoints, matM);
    cvConvert(imagePoints, _m);
    cvConvert(A, &matA);

    CV_Assert((CV_MAT_DEPTH(rvec->type) == CV_64F
                  || CV_MAT_DEPTH(rvec->type) == CV_32F)
        && (rvec->rows == 1 || rvec->cols == 1)
        && rvec->rows * rvec->cols * CV_MAT_CN(rvec->type) == 3);

    CV_Assert((CV_MAT_DEPTH(tvec->type) == CV_64F
                  || CV_MAT_DEPTH(tvec->type) == CV_32F)
        && (tvec->rows == 1 || tvec->cols == 1)
        && tvec->rows * tvec->cols * CV_MAT_CN(tvec->type) == 3);

    // it is unsafe to call LM optimisation without an extrinsic guess
    // in the case of 3 points. This is because there is no guarantee
    // that it will converge on the correct solution.
    CV_Assert((count >= 4) || (count == 3 && useExtrinsicGuess));

    _mn.reset(cvCreateMat(1, count, CV_64FC2));
    _Mxy.reset(cvCreateMat(1, count, CV_64FC2));

    // normalize image points
    // (unapply the intrinsic matrix transformation and distortion)
    cvUndistortPoints(_m, _mn, &matA, distCoeffs, 0, &_Ar);

    if (useExtrinsicGuess) {
        CvMat _r_temp = cvMat(rvec->rows, rvec->cols,
            CV_MAKETYPE(CV_64F, CV_MAT_CN(rvec->type)), param);
        CvMat _t_temp = cvMat(tvec->rows, tvec->cols,
            CV_MAKETYPE(CV_64F, CV_MAT_CN(tvec->type)), param + 3);
        cvConvert(rvec, &_r_temp);
        cvConvert(tvec, &_t_temp);
    } else {
        Mc = cvAvg(matM);
        cvReshape(matM, matM, 1, count);
        cvMulTransposed(matM, &_MM, 1, &_Mc);
        cvSVD(&_MM, &matW, 0, &matV, CV_SVD_MODIFY_A + CV_SVD_V_T);

        // initialize extrinsic parameters
        if (W[2] / W[1] < 1e-3) {
            // a planar structure case (all M's lie in the same plane)
            double tt[3], h[9], h1_norm, h2_norm;
            CvMat* R_transform = &matV;
            CvMat T_transform = cvMat(3, 1, CV_64F, tt);
            CvMat matH = cvMat(3, 3, CV_64F, h);
            CvMat _h1, _h2, _h3;

            if (V[2] * V[2] + V[5] * V[5] < 1e-10)
                cvSetIdentity(R_transform);

            if (cvDet(R_transform) < 0)
                cvScale(R_transform, R_transform, -1);

            cvGEMM(R_transform, &_Mc, -1, 0, 0, &T_transform, CV_GEMM_B_T);

            for (i = 0; i < count; i++) {
                const double* Rp = R_transform->data.db;
                const double* Tp = T_transform.data.db;
                const double* src = matM->data.db + i * 3;
                double* dst = _Mxy->data.db + i * 2;

                dst[0] = Rp[0] * src[0] + Rp[1] * src[1] + Rp[2] * src[2]
                    + Tp[0];
                dst[1] = Rp[3] * src[0] + Rp[4] * src[1] + Rp[5] * src[2]
                    + Tp[1];
            }

            cvFindHomography(_Mxy, _mn, &matH);

            if (cvCheckArr(&matH, CV_CHECK_QUIET)) {
                cvGetCol(&matH, &_h1, 0);
                _h2 = _h1;
                _h2.data.db++;
                _h3 = _h2;
                _h3.data.db++;
                h1_norm = std::sqrt(h[0] * h[0] + h[3] * h[3] + h[6] * h[6]);
                h2_norm = std::sqrt(h[1] * h[1] + h[4] * h[4] + h[7] * h[7]);

                cvScale(&_h1, &_h1, 1. / MAX(h1_norm, DBL_EPSILON));
                cvScale(&_h2, &_h2, 1. / MAX(h2_norm, DBL_EPSILON));
                cvScale(&_h3, &_t, 2. / MAX(h1_norm + h2_norm, DBL_EPSILON));
                cvCrossProduct(&_h1, &_h2, &_h3);

                cvRodrigues2(&matH, &_r);
                cvRodrigues2(&_r, &matH);
                cvMatMulAdd(&matH, &T_transform, &_t, &_t);
                cvMatMul(&matH, R_transform, &matR);
            } else {
                cvSetIdentity(&matR);
                cvZero(&_t);
            }

            cvRodrigues2(&matR, &_r);
        } else {
            // non-planar structure. Use DLT method
            double* L;
            double LL[12 * 12], LW[12], LV[12 * 12], sc;
            CvMat _LL = cvMat(12, 12, CV_64F, LL);
            CvMat _LW = cvMat(12, 1, CV_64F, LW);
            CvMat _LV = cvMat(12, 12, CV_64F, LV);
            CvMat _RRt, _RR, _tt;
            CvPoint3D64f* M = (CvPoint3D64f*)matM->data.db;
            CvPoint2D64f* mn = (CvPoint2D64f*)_mn->data.db;

            matL.reset(cvCreateMat(2 * count, 12, CV_64F));
            L = matL->data.db;

            for (i = 0; i < count; i++, L += 24) {
                double x = -mn[i].x, y = -mn[i].y;
                L[0] = L[16] = M[i].x;
                L[1] = L[17] = M[i].y;
                L[2] = L[18] = M[i].z;
                L[3] = L[19] = 1.;
                L[4] = L[5] = L[6] = L[7] = 0.;
                L[12] = L[13] = L[14] = L[15] = 0.;
                L[8] = x * M[i].x;
                L[9] = x * M[i].y;
                L[10] = x * M[i].z;
                L[11] = x;
                L[20] = y * M[i].x;
                L[21] = y * M[i].y;
                L[22] = y * M[i].z;
                L[23] = y;
            }

            cvMulTransposed(matL, &_LL, 1);
            cvSVD(&_LL, &_LW, 0, &_LV, CV_SVD_MODIFY_A + CV_SVD_V_T);
            _RRt = cvMat(3, 4, CV_64F, LV + 11 * 12);
            cvGetCols(&_RRt, &_RR, 0, 3);
            cvGetCol(&_RRt, &_tt, 3);
            if (cvDet(&_RR) < 0)
                cvScale(&_RRt, &_RRt, -1);
            sc = cvNorm(&_RR);
            cvSVD(&_RR, &matW, &matU, &matV,
                CV_SVD_MODIFY_A + CV_SVD_U_T + CV_SVD_V_T);
            cvGEMM(&matU, &matV, 1, 0, 0, &matR, CV_GEMM_A_T);
            cvScale(&_tt, &_t, cvNorm(&matR) / sc);
            cvRodrigues2(&matR, &_r);
        }
    }

    cvReshape(matM, matM, 3, 1);
    cvReshape(_mn, _mn, 2, 1);

    // refine extrinsic parameters using iterative algorithm
    CvLevMarq solver(6, count * 2,
        cvTermCriteria(
            CV_TERMCRIT_EPS + CV_TERMCRIT_ITER, max_iter, FLT_EPSILON),
        true);
    cvCopy(&_param, solver.param);

    for (;;) {
        CvMat *matJ = 0, *_err = 0;
        const CvMat* __param = 0;
        bool proceed = solver.update(__param, matJ, _err);
        cvCopy(__param, &_param);
        if (!proceed || !_err)
            break;
        cvReshape(_err, _err, 2, 1);
        if (matJ) {
            cvGetCols(matJ, &_dpdr, 0, 3);
            cvGetCols(matJ, &_dpdt, 3, 6);
            projectPoints2(matM, &_r, &_t, &matA, distCoeffs, _err, &_dpdr,
                &_dpdt, 0, 0, 0, 0);
        } else {
            projectPoints2(
                matM, &_r, &_t, &matA, distCoeffs, _err, 0, 0, 0, 0, 0, 0);
        }
        cvSub(_err, _m, _err);
        cvReshape(_err, _err, 1, 2 * count);
    }
    cvCopy(solver.param, &_param);

    _r = cvMat(rvec->rows, rvec->cols,
        CV_MAKETYPE(CV_64F, CV_MAT_CN(rvec->type)), param);
    _t = cvMat(tvec->rows, tvec->cols,
        CV_MAKETYPE(CV_64F, CV_MAT_CN(tvec->type)), param + 3);

    cvConvert(&_r, rvec);
    cvConvert(&_t, tvec);
}

static void initIntrinsicParams2D(const CvMat* objectPoints,
    const CvMat* imagePoints, const CvMat* npoints, CvSize imageSize,
    CvMat* cameraMatrix, double aspectRatio)
{
    Ptr<CvMat> matA, _b, _allH;

    int i, j, pos, nimages, ni = 0;
    double a[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    double H[9] = { 0 }, f[2] = { 0 };
    CvMat _a = cvMat(3, 3, CV_64F, a);
    CvMat matH = cvMat(3, 3, CV_64F, H);
    CvMat _f = cvMat(2, 1, CV_64F, f);

    assert(CV_MAT_TYPE(npoints->type) == CV_32SC1
        && CV_IS_MAT_CONT(npoints->type));
    nimages = npoints->rows * npoints->cols;

    if ((CV_MAT_TYPE(objectPoints->type) != CV_32FC3
            && CV_MAT_TYPE(objectPoints->type) != CV_64FC3)
        || (CV_MAT_TYPE(imagePoints->type) != CV_32FC2
               && CV_MAT_TYPE(imagePoints->type) != CV_64FC2))
        CV_Error(CV_StsUnsupportedFormat,
            "Object points must be 3D and image points must be 2D");

    if (objectPoints->rows != 1 || imagePoints->rows != 1)
        CV_Error(CV_StsBadSize,
            "object points and image points must be a single-row matrices");

    matA.reset(cvCreateMat(2 * nimages, 2, CV_64F));
    _b.reset(cvCreateMat(2 * nimages, 1, CV_64F));
    a[2] = (!imageSize.width) ? 0.5 : (imageSize.width) * 0.5;
    a[5] = (!imageSize.height) ? 0.5 : (imageSize.height) * 0.5;
    _allH.reset(cvCreateMat(nimages, 9, CV_64F));

    // extract vanishing points in order to obtain initial value for the focal
    // length
    for (i = 0, pos = 0; i < nimages; i++, pos += ni) {
        double* Ap = matA->data.db + i * 4;
        double* bp = _b->data.db + i * 2;
        ni = npoints->data.i[i];
        double h[3], v[3], d1[3], d2[3];
        double n[4] = { 0, 0, 0, 0 };
        CvMat _m, matM;
        cvGetCols(objectPoints, &matM, 0, ni);
        cvGetCols(imagePoints, &_m, pos, pos + ni);

        cvFindHomography(&matM, &_m, &matH);
        memcpy(_allH->data.db + i * 9, H, sizeof(H));

        H[0] -= H[6] * a[2];
        H[1] -= H[7] * a[2];
        H[2] -= H[8] * a[2];
        H[3] -= H[6] * a[5];
        H[4] -= H[7] * a[5];
        H[5] -= H[8] * a[5];

        for (j = 0; j < 3; j++) {
            double t0 = H[j * 3], t1 = H[j * 3 + 1];
            h[j] = t0;
            v[j] = t1;
            d1[j] = (t0 + t1) * 0.5;
            d2[j] = (t0 - t1) * 0.5;
            n[0] += t0 * t0;
            n[1] += t1 * t1;
            n[2] += d1[j] * d1[j];
            n[3] += d2[j] * d2[j];
        }

        for (j = 0; j < 4; j++)
            n[j] = 1. / std::sqrt(n[j]);

        for (j = 0; j < 3; j++) {
            h[j] *= n[0];
            v[j] *= n[1];
            d1[j] *= n[2];
            d2[j] *= n[3];
        }

        Ap[0] = h[0] * v[0];
        Ap[1] = h[1] * v[1];
        Ap[2] = d1[0] * d2[0];
        Ap[3] = d1[1] * d2[1];
        bp[0] = -h[2] * v[2];
        bp[1] = -d1[2] * d2[2];
    }

    cvSolve(matA, _b, &_f, CV_NORMAL + CV_SVD);
    a[0] = std::sqrt(fabs(1. / f[0]));
    a[4] = std::sqrt(fabs(1. / f[1]));
    if (aspectRatio != 0) {
        double tf = (a[0] + a[4]) / (aspectRatio + 1.);
        a[0] = aspectRatio * tf;
        a[4] = tf;
    }

    cvConvert(&_a, cameraMatrix);
}

static void subMatrix(const cv::Mat& src, cv::Mat& dst,
    const std::vector<uchar>& cols, const std::vector<uchar>& rows)
{
    int nonzeros_cols = cv::countNonZero(cols);
    cv::Mat tmp(src.rows, nonzeros_cols, CV_64FC1);

    for (int i = 0, j = 0; i < (int)cols.size(); i++) {
        if (cols[i]) {
            src.col(i).copyTo(tmp.col(j++));
        }
    }

    int nonzeros_rows = cv::countNonZero(rows);
    dst.create(nonzeros_rows, nonzeros_cols, CV_64FC1);
    for (int i = 0, j = 0; i < (int)rows.size(); i++) {
        if (rows[i]) {
            tmp.row(i).copyTo(dst.row(j++));
        }
    }
}

static double calibrateCamera2Internal(const CvMat* objectPoints,
    const CvMat* imagePoints, const CvMat* npoints, CvSize imageSize,
    int fixedObjPt, CvMat* cameraMatrix, CvMat* distCoeffs,
    CvMat* newObjPoints, CvMat* rvecs, CvMat* tvecs, CvMat* stdDevs,
    CvMat* perViewErrors, int flags, CvTermCriteria termCrit)
{
    const int NINTRINSIC = CV_CALIB_NINTRINSIC;
    double reprojErr = 0;

    Matx33d A;
    double k[14] = { 0 };
    CvMat matA = cvMat(3, 3, CV_64F, A.val), _k;
    int i, nimages, maxPoints = 0, ni = 0, pos, total = 0, nparams, npstep,
                    cn;
    double aspectRatio = 0.;

    // 0. check the parameters & allocate buffers
    if (!CV_IS_MAT(objectPoints) || !CV_IS_MAT(imagePoints)
        || !CV_IS_MAT(npoints) || !CV_IS_MAT(cameraMatrix)
        || !CV_IS_MAT(distCoeffs))
        CV_Error(CV_StsBadArg,
            "One of required vector arguments is not a valid matrix");

    if (imageSize.width <= 0 || imageSize.height <= 0)
        CV_Error(CV_StsOutOfRange, "image width and height must be positive");

    if (CV_MAT_TYPE(npoints->type) != CV_32SC1
        || (npoints->rows != 1 && npoints->cols != 1))
        CV_Error(CV_StsUnsupportedFormat, "the array of point counters must "
                                          "be 1-dimensional integer vector");
    if (flags & CALIB_TILTED_MODEL) {
        // when the tilted sensor model is used the distortion coefficients
        // matrix must have 14 parameters
        if (distCoeffs->cols * distCoeffs->rows != 14)
            CV_Error(CV_StsBadArg, "The tilted sensor model must have 14 "
                                   "parameters in the distortion matrix");
    } else {
        // when the thin prism model is used the distortion coefficients
        // matrix must have 12 parameters
        if (flags & CALIB_THIN_PRISM_MODEL)
            if (distCoeffs->cols * distCoeffs->rows != 12)
                CV_Error(CV_StsBadArg, "Thin prism model must have 12 "
                                       "parameters in the distortion matrix");
    }

    nimages = npoints->rows * npoints->cols;
    npstep = npoints->rows == 1 ? 1
                                : npoints->step / CV_ELEM_SIZE(npoints->type);

    if (rvecs) {
        cn = CV_MAT_CN(rvecs->type);
        if (!CV_IS_MAT(rvecs) || (CV_MAT_DEPTH(rvecs->type) != CV_32F
                                     && CV_MAT_DEPTH(rvecs->type) != CV_64F)
            || ((rvecs->rows != nimages
                    || (rvecs->cols * cn != 3 && rvecs->cols * cn != 9))
                   && (rvecs->rows != 1 || rvecs->cols != nimages
                          || cn != 3)))
            CV_Error(CV_StsBadArg,
                "the output array of rotation vectors must be 3-channel "
                "1xn or nx1 array or 1-channel nx3 or nx9 array, where n is "
                "the number of views");
    }

    if (tvecs) {
        cn = CV_MAT_CN(tvecs->type);
        if (!CV_IS_MAT(tvecs) || (CV_MAT_DEPTH(tvecs->type) != CV_32F
                                     && CV_MAT_DEPTH(tvecs->type) != CV_64F)
            || ((tvecs->rows != nimages || tvecs->cols * cn != 3)
                   && (tvecs->rows != 1 || tvecs->cols != nimages
                          || cn != 3)))
            CV_Error(CV_StsBadArg,
                "the output array of translation vectors must be 3-channel "
                "1xn or nx1 array or 1-channel nx3 array, where n is the "
                "number of views");
    }

    if (stdDevs) {
        cn = CV_MAT_CN(stdDevs->type);
        if (!CV_IS_MAT(stdDevs)
            || (CV_MAT_DEPTH(stdDevs->type) != CV_32F
                   && CV_MAT_DEPTH(stdDevs->type) != CV_64F)
            || ((stdDevs->rows != (nimages * 6 + NINTRINSIC + maxPoints * 3)
                    || stdDevs->cols * cn != 1)
                   && (stdDevs->rows != 1
                          || stdDevs->cols
                              != (nimages * 6 + NINTRINSIC + maxPoints * 3)
                          || cn != 1)))
#define STR__(x) #x
#define STR_(x) STR__(x)
            CV_Error(CV_StsBadArg,
                "the output array of standard deviations vectors must be "
                "1-channel 1x(n*6 + NINTRINSIC + maxPoints*3) or (n*6 + "
                "NINTRINSIC + maxPoints*3)x1 array, where n is the number of "
                "views, maxPoints is the number of points per view, "
                "NINTRINSIC = " STR_(CV_CALIB_NINTRINSIC));
    }

    if ((CV_MAT_TYPE(cameraMatrix->type) != CV_32FC1
            && CV_MAT_TYPE(cameraMatrix->type) != CV_64FC1)
        || cameraMatrix->rows != 3 || cameraMatrix->cols != 3)
        CV_Error(CV_StsBadArg,
            "Intrinsic parameters must be 3x3 floating-point matrix");

    if ((CV_MAT_TYPE(distCoeffs->type) != CV_32FC1
            && CV_MAT_TYPE(distCoeffs->type) != CV_64FC1)
        || (distCoeffs->cols != 1 && distCoeffs->rows != 1)
        || (distCoeffs->cols * distCoeffs->rows != 4
               && distCoeffs->cols * distCoeffs->rows != 5
               && distCoeffs->cols * distCoeffs->rows != 8
               && distCoeffs->cols * distCoeffs->rows != 12
               && distCoeffs->cols * distCoeffs->rows != 14))
        CV_Error(CV_StsBadArg, cvDistCoeffErr);

    for (i = 0; i < nimages; i++) {
        ni = npoints->data.i[i * npstep];
        if (ni < 4) {
            CV_Error_(CV_StsOutOfRange,
                ("The number of points in the view #%d is < 4", i));
        }
        maxPoints = MAX(maxPoints, ni);
        total += ni;
    }

    Mat matM(1, maxPoints, CV_64FC3);
    Mat _m(1, total, CV_64FC2);
    Mat allErrors(1, total, CV_64FC2);

    if (CV_MAT_CN(objectPoints->type) == 3) {
        cvarrToMat(objectPoints).convertTo(matM, CV_64F);
    } else {
        convertPointsHomogeneous(cvarrToMat(objectPoints), matM);
    }

    if (CV_MAT_CN(imagePoints->type) == 2) {
        cvarrToMat(imagePoints).convertTo(_m, CV_64F);
    } else {
        convertPointsHomogeneous(cvarrToMat(imagePoints), _m);
    }

    nparams = NINTRINSIC + nimages * 6 + maxPoints * 3;
    Mat _Ji(maxPoints * 2, NINTRINSIC, CV_64FC1, Scalar(0));
    Mat _Je(maxPoints * 2, 6, CV_64FC1);
    Mat _Jo(maxPoints * 2, maxPoints * 3, CV_64FC1, Scalar(0));
    Mat _err(maxPoints * 2, 1, CV_64FC1);

    _k = cvMat(distCoeffs->rows, distCoeffs->cols,
        CV_MAKETYPE(CV_64F, CV_MAT_CN(distCoeffs->type)), k);
    if (distCoeffs->rows * distCoeffs->cols * CV_MAT_CN(distCoeffs->type)
        < 8) {
        if (distCoeffs->rows * distCoeffs->cols * CV_MAT_CN(distCoeffs->type)
            < 5)
            flags |= CALIB_FIX_K3;
        flags |= CALIB_FIX_K4 | CALIB_FIX_K5 | CALIB_FIX_K6;
    }
    const double minValidAspectRatio = 0.01;
    const double maxValidAspectRatio = 100.0;

    // 1. initialize intrinsic parameters & LM solver
    if (flags & CALIB_USE_INTRINSIC_GUESS) {
        cvConvert(cameraMatrix, &matA);
        if (A(0, 0) <= 0 || A(1, 1) <= 0)
            CV_Error(CV_StsOutOfRange,
                "Focal length (fx and fy) must be positive");
        if (A(0, 2) < 0 || A(0, 2) >= imageSize.width || A(1, 2) < 0
            || A(1, 2) >= imageSize.height)
            CV_Error(
                CV_StsOutOfRange, "Principal point must be within the image");
        if (fabs(A(0, 1)) > 1e-5)
            CV_Error(CV_StsOutOfRange,
                "Non-zero skew is not supported by the function");
        if (fabs(A(1, 0)) > 1e-5 || fabs(A(2, 0)) > 1e-5
            || fabs(A(2, 1)) > 1e-5 || fabs(A(2, 2) - 1) > 1e-5)
            CV_Error(CV_StsOutOfRange, "The intrinsic matrix must have [fx 0 "
                                       "cx; 0 fy cy; 0 0 1] shape");
        A(0, 1) = A(1, 0) = A(2, 0) = A(2, 1) = 0.;
        A(2, 2) = 1.;

        if (flags & CALIB_FIX_ASPECT_RATIO) {
            aspectRatio = A(0, 0) / A(1, 1);

            if (aspectRatio < minValidAspectRatio
                || aspectRatio > maxValidAspectRatio)
                CV_Error(CV_StsOutOfRange,
                    "The specified aspect ratio (= cameraMatrix[0][0] / "
                    "cameraMatrix[1][1]) is incorrect");
        }
        cvConvert(distCoeffs, &_k);
    } else {
        Scalar mean, sdv;
        meanStdDev(matM, mean, sdv);
        if (fabs(mean[2]) > 1e-5 || fabs(sdv[2]) > 1e-5)
            CV_Error(CV_StsBadArg, "For non-planar calibration rigs the "
                                   "initial intrinsic matrix must be "
                                   "specified");
        for (i = 0; i < maxPoints; i++)
            matM.at<Point3d>(i).z = 0.;

        if (flags & CALIB_FIX_ASPECT_RATIO) {
            aspectRatio = cvmGet(cameraMatrix, 0, 0);
            aspectRatio /= cvmGet(cameraMatrix, 1, 1);
            if (aspectRatio < minValidAspectRatio
                || aspectRatio > maxValidAspectRatio)
                CV_Error(CV_StsOutOfRange,
                    "The specified aspect ratio (= cameraMatrix[0][0] / "
                    "cameraMatrix[1][1]) is incorrect");
        }
        CvMat _matM = CvMat(matM), m = CvMat(_m);
        initIntrinsicParams2D(
            &_matM, &m, npoints, imageSize, &matA, aspectRatio);
    }

    CvLevMarq solver(nparams, 0, termCrit);

    if (flags & CALIB_USE_LU) {
        solver.solveMethod = DECOMP_LU;
    } else if (flags & CALIB_USE_QR) {
        solver.solveMethod = DECOMP_QR;
    }

    {
        double* param = solver.param->data.db;
        uchar* mask = solver.mask->data.ptr;

        param[0] = A(0, 0);
        param[1] = A(1, 1);
        param[2] = A(0, 2);
        param[3] = A(1, 2);
        std::copy(k, k + 14, param + 4);

        if (flags & CALIB_FIX_ASPECT_RATIO)
            mask[0] = 0;
        if (flags & CALIB_FIX_FOCAL_LENGTH)
            mask[0] = mask[1] = 0;
        if (flags & CALIB_FIX_PRINCIPAL_POINT)
            mask[2] = mask[3] = 0;
        if (flags & CALIB_ZERO_TANGENT_DIST) {
            param[6] = param[7] = 0;
            mask[6] = mask[7] = 0;
        }
        if (!(flags & CALIB_RATIONAL_MODEL))
            flags |= CALIB_FIX_K4 + CALIB_FIX_K5 + CALIB_FIX_K6;
        if (!(flags & CALIB_THIN_PRISM_MODEL))
            flags |= CALIB_FIX_S1_S2_S3_S4;
        if (!(flags & CALIB_TILTED_MODEL))
            flags |= CALIB_FIX_TAUX_TAUY;

        mask[4] = !(flags & CALIB_FIX_K1);
        mask[5] = !(flags & CALIB_FIX_K2);
        if (flags & CALIB_FIX_TANGENT_DIST) {
            mask[6] = mask[7] = 0;
        }
        mask[8] = !(flags & CALIB_FIX_K3);
        mask[9] = !(flags & CALIB_FIX_K4);
        mask[10] = !(flags & CALIB_FIX_K5);
        mask[11] = !(flags & CALIB_FIX_K6);

        if (flags & CALIB_FIX_S1_S2_S3_S4) {
            mask[12] = 0;
            mask[13] = 0;
            mask[14] = 0;
            mask[15] = 0;
        }
        if (flags & CALIB_FIX_TAUX_TAUY) {
            mask[16] = 0;
            mask[17] = 0;
        }

        // copy object points
        std::copy(matM.ptr<double>(), matM.ptr<double>(0, maxPoints - 1) + 3,
            param + NINTRINSIC + nimages * 6);
        // fix points
        mask[NINTRINSIC + nimages * 6] = 0;
        mask[NINTRINSIC + nimages * 6 + 1] = 0;
        mask[NINTRINSIC + nimages * 6 + 2] = 0;
        mask[NINTRINSIC + nimages * 6 + fixedObjPt * 3] = 0;
        mask[NINTRINSIC + nimages * 6 + fixedObjPt * 3 + 1] = 0;
        mask[NINTRINSIC + nimages * 6 + fixedObjPt * 3 + 2] = 0;
        mask[nparams - 1] = 0;
    }

    // 2. initialize extrinsic parameters
    CvMat _Mi(matM.colRange(0, maxPoints));
    for (i = 0, pos = 0; i < nimages; i++, pos += ni) {
        CvMat _ri, _ti;
        ni = npoints->data.i[i * npstep];

        cvGetRows(
            solver.param, &_ri, NINTRINSIC + i * 6, NINTRINSIC + i * 6 + 3);
        cvGetRows(solver.param, &_ti, NINTRINSIC + i * 6 + 3,
            NINTRINSIC + i * 6 + 6);

        CvMat _mi(_m.colRange(pos, pos + ni));

        findExtrinsicCameraParams2(&_Mi, &_mi, &matA, &_k, &_ri, &_ti);
    }

    // 3. run the optimization
    for (;;) {
        const CvMat* _param = 0;
        CvMat *_JtJ = 0, *_JtErr = 0;
        double* _errNorm = 0;
        bool proceed = solver.updateAlt(_param, _JtJ, _JtErr, _errNorm);
        double *param = solver.param->data.db,
               *pparam = solver.prevParam->data.db;
        bool calcJ
            = solver.state == CvLevMarq::CALC_J || (!proceed && stdDevs);

        if (flags & CALIB_FIX_ASPECT_RATIO) {
            param[0] = param[1] * aspectRatio;
            pparam[0] = pparam[1] * aspectRatio;
        }

        A(0, 0) = param[0];
        A(1, 1) = param[1];
        A(0, 2) = param[2];
        A(1, 2) = param[3];
        std::copy(param + 4, param + 4 + 14, k);
        cvGetRows(solver.param, &_Mi, NINTRINSIC + nimages * 6,
            NINTRINSIC + nimages * 6 + ni * 3);
        cvReshape(&_Mi, &_Mi, 3, 1);

        if (!proceed && !stdDevs && !perViewErrors)
            break;
        else if (!proceed && stdDevs)
            cvZero(_JtJ);

        reprojErr = 0;

        for (i = 0, pos = 0; i < nimages; i++, pos += ni) {
            CvMat _ri, _ti;
            ni = npoints->data.i[i * npstep];

            cvGetRows(solver.param, &_ri, NINTRINSIC + i * 6,
                NINTRINSIC + i * 6 + 3);
            cvGetRows(solver.param, &_ti, NINTRINSIC + i * 6 + 3,
                NINTRINSIC + i * 6 + 6);

            CvMat _mi = CvMat(_m.colRange(pos, pos + ni));
            CvMat _me = CvMat(allErrors.colRange(pos, pos + ni));

            _Je.resize(ni * 2);
            _Ji.resize(ni * 2);
            _Jo.resize(ni * 2);
            _err.resize(ni * 2);
            CvMat _dpdr = CvMat(_Je.colRange(0, 3));
            CvMat _dpdt = CvMat(_Je.colRange(3, 6));
            CvMat _dpdf = CvMat(_Ji.colRange(0, 2));
            CvMat _dpdc = CvMat(_Ji.colRange(2, 4));
            CvMat _dpdk = CvMat(_Ji.colRange(4, NINTRINSIC));
            CvMat _dpdo = CvMat(_Jo.colRange(0, maxPoints * 3));
            CvMat _mp = CvMat(_err.reshape(2, 1));

            if (calcJ) {
                projectPoints2(&_Mi, &_ri, &_ti, &matA, &_k, &_mp, &_dpdr,
                    &_dpdt, (flags & CALIB_FIX_FOCAL_LENGTH) ? 0 : &_dpdf,
                    (flags & CALIB_FIX_PRINCIPAL_POINT) ? 0 : &_dpdc, &_dpdk,
                    &_dpdo,
                    (flags & CALIB_FIX_ASPECT_RATIO) ? aspectRatio : 0);
            } else
                projectPoints2(&_Mi, &_ri, &_ti, &matA, &_k, &_mp);

            cvSub(&_mp, &_mi, &_mp);
            if (perViewErrors || stdDevs)
                cvCopy(&_mp, &_me);

            if (calcJ) {
                Mat JtJ(cvarrToMat(_JtJ)), JtErr(cvarrToMat(_JtErr));

                // see HZ: (A6.14) for details on the structure of the
                // Jacobian
                JtJ(Rect(0, 0, NINTRINSIC, NINTRINSIC)) += _Ji.t() * _Ji;
                JtJ(Rect(NINTRINSIC + i * 6, NINTRINSIC + i * 6, 6, 6))
                    = _Je.t() * _Je;
                JtJ(Rect(NINTRINSIC + i * 6, 0, 6, NINTRINSIC))
                    = _Ji.t() * _Je;
                JtJ(Rect(
                    NINTRINSIC + nimages * 6, 0, maxPoints * 3, NINTRINSIC))
                    += _Ji.t() * _Jo;
                JtJ(Rect(NINTRINSIC + nimages * 6, NINTRINSIC + i * 6,
                    maxPoints * 3, 6))
                    += _Je.t() * _Jo;
                JtJ(Rect(NINTRINSIC + nimages * 6, NINTRINSIC + nimages * 6,
                    maxPoints * 3, maxPoints * 3))
                    += _Jo.t() * _Jo;

                JtErr.rowRange(0, NINTRINSIC) += _Ji.t() * _err;
                JtErr.rowRange(NINTRINSIC + i * 6, NINTRINSIC + (i + 1) * 6)
                    = _Je.t() * _err;
                JtErr.rowRange(NINTRINSIC + nimages * 6, nparams)
                    += _Jo.t() * _err;
            }

            double viewErr = norm(_err, NORM_L2SQR);

            if (perViewErrors)
                perViewErrors->data.db[i] = std::sqrt(viewErr / ni);

            reprojErr += viewErr;
        }
        if (_errNorm)
            *_errNorm = reprojErr;

        if (!proceed) {
            if (stdDevs) {
                Mat mask = cvarrToMat(solver.mask);
                int nparams_nz = countNonZero(mask);
                Mat JtJinv, JtJN;
                JtJN.create(nparams_nz, nparams_nz, CV_64F);
                subMatrix(cvarrToMat(_JtJ), JtJN, mask, mask);
                completeSymm(JtJN, false);
                cv::invert(JtJN, JtJinv, DECOMP_SVD);
                // sigma2 is deviation of the noise
                // see any papers about variance of the least squares
                // estimator for
                // detailed description of the variance estimation methods
                double sigma2
                    = norm(allErrors, NORM_L2SQR) / (total - nparams_nz);
                Mat stdDevsM = cvarrToMat(stdDevs);
                int j = 0;
                for (int s = 0; s < nparams; s++)
                    if (mask.data[s]) {
                        stdDevsM.at<double>(s)
                            = std::sqrt(JtJinv.at<double>(j, j) * sigma2);
                        j++;
                    } else
                        stdDevsM.at<double>(s) = 0.;
            }
            break;
        }
    }

    // 4. store the results
    cvConvert(&matA, cameraMatrix);
    cvConvert(&_k, distCoeffs);
    cvConvert(&_Mi, newObjPoints);

    for (i = 0, pos = 0; i < nimages; i++) {
        CvMat src, dst;

        if (rvecs) {
            src = cvMat(
                3, 1, CV_64F, solver.param->data.db + NINTRINSIC + i * 6);
            if (rvecs->rows == nimages
                && rvecs->cols * CV_MAT_CN(rvecs->type) == 9) {
                dst = cvMat(3, 3, CV_MAT_DEPTH(rvecs->type),
                    rvecs->data.ptr + rvecs->step * i);
                cvRodrigues2(&src, &matA);
                cvConvert(&matA, &dst);
            } else {
                dst = cvMat(3, 1, CV_MAT_DEPTH(rvecs->type), rvecs->rows == 1
                        ? rvecs->data.ptr + i * CV_ELEM_SIZE(rvecs->type)
                        : rvecs->data.ptr + rvecs->step * i);
                cvConvert(&src, &dst);
            }
        }
        if (tvecs) {
            src = cvMat(
                3, 1, CV_64F, solver.param->data.db + NINTRINSIC + i * 6 + 3);
            dst = cvMat(3, 1, CV_MAT_DEPTH(tvecs->type), tvecs->rows == 1
                    ? tvecs->data.ptr + i * CV_ELEM_SIZE(tvecs->type)
                    : tvecs->data.ptr + tvecs->step * i);
            cvConvert(&src, &dst);
        }
    }

    return std::sqrt(reprojErr / total);
}

static void collectCalibrationData(InputArray objectPoints,
    InputArrayOfArrays imagePoints1, InputArrayOfArrays imagePoints2,
    Mat& objPtMat, Mat& imgPtMat1, Mat* imgPtMat2, Mat& npoints)
{
    int nimages = (int)imagePoints1.total();
    int i, j = 0, ni = 0, total = 0;
    CV_Assert(
        nimages > 0 && (!imgPtMat2 || nimages == (int)imagePoints2.total()));

    ni = objectPoints.getMat().checkVector(3, CV_32F);
    if (ni <= 0)
        CV_Error(CV_StsUnsupportedFormat,
            "objectPoints should contain vector of points of type Point3f");
    for (i = 0; i < nimages; i++) {
        int ni1 = imagePoints1.getMat(i).checkVector(2, CV_32F);
        if (ni1 <= 0)
            CV_Error(CV_StsUnsupportedFormat, "imagePoints1 should contain "
                                              "vector of vectors of points "
                                              "of type Point2f");
        CV_Assert(ni == ni1);

        total += ni1;
    }

    npoints.create(1, (int)nimages, CV_32S);
    objPtMat.create(1, (int)ni, CV_32FC3);
    imgPtMat1.create(1, (int)total, CV_32FC2);
    Point2f* imgPtData2 = 0;

    if (imgPtMat2) {
        imgPtMat2->create(1, (int)total, CV_32FC2);
        imgPtData2 = imgPtMat2->ptr<Point2f>();
    }

    Point3f* objPtData = objPtMat.ptr<Point3f>();
    Point2f* imgPtData1 = imgPtMat1.ptr<Point2f>();

    Mat objpt = objectPoints.getMat();
    for (int n = 0; n < ni; ++n) {
        objPtData[n] = objpt.ptr<Point3f>()[n];
    }
    for (i = 0; i < nimages; i++, j += ni) {
        Mat imgpt1 = imagePoints1.getMat(i);
        ni = imgpt1.checkVector(2, CV_32F);
        npoints.at<int>(i) = ni;
        for (int n = 0; n < ni; ++n) {
            imgPtData1[j + n] = imgpt1.ptr<Point2f>()[n];
        }

        if (imgPtData2) {
            Mat imgpt2 = imagePoints2.getMat(i);
            int ni2 = imgpt2.checkVector(2, CV_32F);
            CV_Assert(ni == ni2);
            for (int n = 0; n < ni2; ++n) {
                imgPtData2[j + n] = imgpt2.ptr<Point2f>()[n];
            }
        }
    }
}

static Mat prepareCameraMatrix(Mat& cameraMatrix0, int rtype)
{
    Mat cameraMatrix = Mat::eye(3, 3, rtype);
    if (cameraMatrix0.size() == cameraMatrix.size())
        cameraMatrix0.convertTo(cameraMatrix, rtype);
    return cameraMatrix;
}

static Mat prepareDistCoeffs(Mat& distCoeffs0, int rtype, int outputSize = 14)
{
    CV_Assert((int)distCoeffs0.total() <= outputSize);
    Mat distCoeffs = Mat::zeros(
        distCoeffs0.cols == 1 ? Size(1, outputSize) : Size(outputSize, 1),
        rtype);
    if (distCoeffs0.size() == Size(1, 4) || distCoeffs0.size() == Size(1, 5)
        || distCoeffs0.size() == Size(1, 8)
        || distCoeffs0.size() == Size(1, 12)
        || distCoeffs0.size() == Size(1, 14)
        || distCoeffs0.size() == Size(4, 1)
        || distCoeffs0.size() == Size(5, 1)
        || distCoeffs0.size() == Size(8, 1)
        || distCoeffs0.size() == Size(12, 1)
        || distCoeffs0.size() == Size(14, 1)) {
        Mat dstCoeffs(
            distCoeffs, Rect(0, 0, distCoeffs0.cols, distCoeffs0.rows));
        distCoeffs0.convertTo(dstCoeffs, rtype);
    }
    return distCoeffs;
}

double calibrateCamera(InputArrayOfArrays _imagePoints, Size imageSize,
    InputArray _objectPoints, int _fixedObjPt, InputOutputArray _cameraMatrix,
    InputOutputArray _distCoeffs, OutputArrayOfArrays _rvecs,
    OutputArrayOfArrays _tvecs, OutputArray _newObjPoints,
    OutputArray stdDeviationsIntrinsics, OutputArray stdDeviationsExtrinsics,
    OutputArray stdDeviationsObjectPoints, OutputArray _perViewErrors,
    int flags, TermCriteria criteria)
{
    int rtype = CV_64F;
    Mat cameraMatrix = _cameraMatrix.getMat();
    cameraMatrix = prepareCameraMatrix(cameraMatrix, rtype);
    Mat distCoeffs = _distCoeffs.getMat();
    distCoeffs
        = (flags & CALIB_THIN_PRISM_MODEL) && !(flags & CALIB_TILTED_MODEL)
        ? prepareDistCoeffs(distCoeffs, rtype, 12)
        : prepareDistCoeffs(distCoeffs, rtype);
    if (!(flags & CALIB_RATIONAL_MODEL) && (!(flags & CALIB_THIN_PRISM_MODEL))
        && (!(flags & CALIB_TILTED_MODEL)))
        distCoeffs = distCoeffs.rows == 1 ? distCoeffs.colRange(0, 5)
                                          : distCoeffs.rowRange(0, 5);

    int nimages = int(_imagePoints.total());
    CV_Assert(nimages > 0);
    Mat objPt, imgPt, npoints, rvecM, tvecM, stdDeviationsM, errorsM;

    bool rvecs_needed = _rvecs.needed(), tvecs_needed = _tvecs.needed(),
         stddev_needed = stdDeviationsIntrinsics.needed(),
         errors_needed = _perViewErrors.needed(),
         stddev_ext_needed = stdDeviationsExtrinsics.needed();
    bool stddev_obj_needed = stdDeviationsObjectPoints.needed();

    bool rvecs_mat_vec = _rvecs.isMatVector();
    bool tvecs_mat_vec = _tvecs.isMatVector();

    if (rvecs_needed) {
        _rvecs.create(nimages, 1, CV_64FC3);

        if (rvecs_mat_vec)
            rvecM.create(nimages, 3, CV_64F);
        else
            rvecM = _rvecs.getMat();
    }

    if (tvecs_needed) {
        _tvecs.create(nimages, 1, CV_64FC3);

        if (tvecs_mat_vec)
            tvecM.create(nimages, 3, CV_64F);
        else
            tvecM = _tvecs.getMat();
    }

    if (errors_needed) {
        _perViewErrors.create(nimages, 1, CV_64F);
        errorsM = _perViewErrors.getMat();
    }

    collectCalibrationData(
        _objectPoints, _imagePoints, noArray(), objPt, imgPt, 0, npoints);
    Mat newObjPt = _newObjPoints.getMat();
    newObjPt.create(1, objPt.checkVector(3, CV_32F), CV_32FC3);

    bool stddev_any_needed
        = stddev_needed || stddev_ext_needed || stddev_obj_needed;
    int np = npoints.at<int>(0);
    if (stddev_any_needed) {
        stdDeviationsM.create(
            nimages * 6 + CV_CALIB_NINTRINSIC + np * 3, 1, CV_64F);
    }

    CvMat c_objPt = CvMat(objPt), c_imgPt = CvMat(imgPt),
          c_npoints = CvMat(npoints);
    CvMat c_cameraMatrix = CvMat(cameraMatrix),
          c_distCoeffs = CvMat(distCoeffs);
    CvMat c_rvecM = CvMat(rvecM), c_tvecM = CvMat(tvecM),
          c_stdDev = CvMat(stdDeviationsM), c_errors = CvMat(errorsM);
    CvMat c_newObjPt = CvMat(newObjPt);

    double reprojErr = calibrateCamera2Internal(&c_objPt, &c_imgPt,
        &c_npoints, CvSize(imageSize), _fixedObjPt, &c_cameraMatrix,
        &c_distCoeffs, &c_newObjPt, rvecs_needed ? &c_rvecM : NULL,
        tvecs_needed ? &c_tvecM : NULL, stddev_any_needed ? &c_stdDev : NULL,
        errors_needed ? &c_errors : NULL, flags, CvTermCriteria(criteria));

    if (stddev_needed) {
        stdDeviationsIntrinsics.create(CV_CALIB_NINTRINSIC, 1, CV_64F);
        Mat stdDeviationsIntrinsicsMat = stdDeviationsIntrinsics.getMat();
        std::memcpy(stdDeviationsIntrinsicsMat.ptr(), stdDeviationsM.ptr(),
            CV_CALIB_NINTRINSIC * sizeof(double));
    }

    if (stddev_ext_needed) {
        stdDeviationsExtrinsics.create(nimages * 6, 1, CV_64F);
        Mat stdDeviationsExtrinsicsMat = stdDeviationsExtrinsics.getMat();
        std::memcpy(stdDeviationsExtrinsicsMat.ptr(),
            stdDeviationsM.ptr() + CV_CALIB_NINTRINSIC * sizeof(double),
            nimages * 6 * sizeof(double));
    }

    if (stddev_obj_needed) {
        stdDeviationsObjectPoints.create(np * 3, 1, CV_64F);
        Mat stdDeviationsObjectPointsMat = stdDeviationsObjectPoints.getMat();
        std::memcpy(stdDeviationsObjectPointsMat.ptr(), stdDeviationsM.ptr()
                + (CV_CALIB_NINTRINSIC + nimages * 6) * sizeof(double),
            np * 3 * sizeof(double));
    }

    // overly complicated and inefficient rvec/ tvec handling to support
    // vector<Mat>
    for (int i = 0; i < nimages; i++) {
        if (rvecs_needed && rvecs_mat_vec) {
            _rvecs.create(3, 1, CV_64F, i, true);
            Mat rv = _rvecs.getMat(i);
            memcpy(rv.ptr(), rvecM.ptr(i), 3 * sizeof(double));
        }
        if (tvecs_needed && tvecs_mat_vec) {
            _tvecs.create(3, 1, CV_64F, i, true);
            Mat tv = _tvecs.getMat(i);
            memcpy(tv.ptr(), tvecM.ptr(i), 3 * sizeof(double));
        }
    }

    cameraMatrix.copyTo(_cameraMatrix);
    distCoeffs.copyTo(_distCoeffs);
    newObjPt.copyTo(_newObjPoints);

    return reprojErr;
}

double calibrateCamera(InputArrayOfArrays _imagePoints, Size imageSize,
    InputArray _objectPoints, int _fixedObjPt, InputOutputArray _cameraMatrix,
    InputOutputArray _distCoeffs, OutputArrayOfArrays _rvecs,
    OutputArrayOfArrays _tvecs, OutputArray _newObjPoints, int flags,
    TermCriteria criteria)
{
    return calibrateCamera(_imagePoints, imageSize, _objectPoints,
        _fixedObjPt, _cameraMatrix, _distCoeffs, _rvecs, _tvecs,
        _newObjPoints, noArray(), noArray(), noArray(), noArray(), flags,
        criteria);
}

/////////////////////////// Stereo Calibration ///////////////////////////////

static int dbCmp(const void* _a, const void* _b)
{
    double a = *(const double*)_a;
    double b = *(const double*)_b;

    return (a > b) - (a < b);
}

static double stereoCalibrateImpl(const CvMat* _objectPoints,
    const CvMat* _imagePoints1, const CvMat* _imagePoints2,
    const CvMat* _npoints, CvMat* _cameraMatrix1, CvMat* _distCoeffs1,
    CvMat* _cameraMatrix2, CvMat* _distCoeffs2, CvSize imageSize, CvMat* matR,
    CvMat* matT, CvMat* matE, CvMat* matF, CvMat* perViewErr, int flags,
    CvTermCriteria termCrit)
{
    const int NINTRINSIC = 18;
    Ptr<CvMat> npoints, imagePoints[2], objectPoints, RT0;
    double reprojErr = 0;

    double A[2][9], dk[2][14] = { { 0 } }, rlr[9];
    CvMat K[2], Dist[2], om_LR, T_LR;
    CvMat R_LR = cvMat(3, 3, CV_64F, rlr);
    int i, k, p, ni = 0, ofs, nimages, pointsTotal, maxPoints = 0;
    int nparams;
    bool recomputeIntrinsics = false;
    double aspectRatio[2] = { 0 };

    CV_Assert(CV_IS_MAT(_imagePoints1) && CV_IS_MAT(_imagePoints2)
        && CV_IS_MAT(_objectPoints) && CV_IS_MAT(_npoints) && CV_IS_MAT(matR)
        && CV_IS_MAT(matT));

    CV_Assert(CV_ARE_TYPES_EQ(_imagePoints1, _imagePoints2)
        && CV_ARE_DEPTHS_EQ(_imagePoints1, _objectPoints));

    CV_Assert((_npoints->cols == 1 || _npoints->rows == 1)
        && CV_MAT_TYPE(_npoints->type) == CV_32SC1);

    nimages = _npoints->cols + _npoints->rows - 1;
    npoints.reset(
        cvCreateMat(_npoints->rows, _npoints->cols, _npoints->type));
    cvCopy(_npoints, npoints);

    for (i = 0, pointsTotal = 0; i < nimages; i++) {
        maxPoints = MAX(maxPoints, npoints->data.i[i]);
        pointsTotal += npoints->data.i[i];
    }

    objectPoints.reset(cvCreateMat(_objectPoints->rows, _objectPoints->cols,
        CV_64FC(CV_MAT_CN(_objectPoints->type))));
    cvConvert(_objectPoints, objectPoints);
    cvReshape(objectPoints, objectPoints, 3, 1);

    for (k = 0; k < 2; k++) {
        const CvMat* points = k == 0 ? _imagePoints1 : _imagePoints2;
        const CvMat* cameraMatrix = k == 0 ? _cameraMatrix1 : _cameraMatrix2;
        const CvMat* distCoeffs = k == 0 ? _distCoeffs1 : _distCoeffs2;

        int cn = CV_MAT_CN(_imagePoints1->type);
        CV_Assert((CV_MAT_DEPTH(_imagePoints1->type) == CV_32F
                      || CV_MAT_DEPTH(_imagePoints1->type) == CV_64F)
            && ((_imagePoints1->rows == pointsTotal
                    && _imagePoints1->cols * cn == 2)
                   || (_imagePoints1->rows == 1
                          && _imagePoints1->cols == pointsTotal && cn == 2)));

        K[k] = cvMat(3, 3, CV_64F, A[k]);
        Dist[k] = cvMat(1, 14, CV_64F, dk[k]);

        imagePoints[k].reset(cvCreateMat(
            points->rows, points->cols, CV_64FC(CV_MAT_CN(points->type))));
        cvConvert(points, imagePoints[k]);
        cvReshape(imagePoints[k], imagePoints[k], 2, 1);

        if (flags & (CALIB_FIX_INTRINSIC | CALIB_USE_INTRINSIC_GUESS
                        | CALIB_FIX_ASPECT_RATIO | CALIB_FIX_FOCAL_LENGTH))
            cvConvert(cameraMatrix, &K[k]);

        if (flags
            & (CALIB_FIX_INTRINSIC | CALIB_USE_INTRINSIC_GUESS | CALIB_FIX_K1
                  | CALIB_FIX_K2 | CALIB_FIX_K3 | CALIB_FIX_K4 | CALIB_FIX_K5
                  | CALIB_FIX_K6 | CALIB_FIX_TANGENT_DIST)) {
            CvMat tdist = cvMat(distCoeffs->rows, distCoeffs->cols,
                CV_MAKETYPE(CV_64F, CV_MAT_CN(distCoeffs->type)),
                Dist[k].data.db);
            cvConvert(distCoeffs, &tdist);
        }

        if (!(flags & (CALIB_FIX_INTRINSIC | CALIB_USE_INTRINSIC_GUESS))) {
            cvCalibrateCamera2(objectPoints, imagePoints[k], npoints,
                imageSize, &K[k], &Dist[k], NULL, NULL, flags);
        }
    }

    if (flags & CALIB_SAME_FOCAL_LENGTH) {
        static const int avg_idx[] = { 0, 4, 2, 5, -1 };
        for (k = 0; avg_idx[k] >= 0; k++)
            A[0][avg_idx[k]] = A[1][avg_idx[k]]
                = (A[0][avg_idx[k]] + A[1][avg_idx[k]]) * 0.5;
    }

    if (flags & CALIB_FIX_ASPECT_RATIO) {
        for (k = 0; k < 2; k++)
            aspectRatio[k] = A[k][0] / A[k][4];
    }

    recomputeIntrinsics = (flags & CALIB_FIX_INTRINSIC) == 0;

    Mat err(maxPoints * 2, 1, CV_64F);
    Mat Je(maxPoints * 2, 6, CV_64F);
    Mat J_LR(maxPoints * 2, 6, CV_64F);
    Mat Ji(maxPoints * 2, NINTRINSIC, CV_64F, Scalar(0));

    // we optimize for the inter-camera R(3),t(3), then, optionally,
    // for intrinisic parameters of each camera ((fx,fy,cx,cy,k1,k2,p1,p2) ~ 8
    // parameters).
    nparams = 6 * (nimages + 1) + (recomputeIntrinsics ? NINTRINSIC * 2 : 0);

    CvLevMarq solver(nparams, 0, termCrit);

    if (flags & CALIB_USE_LU) {
        solver.solveMethod = DECOMP_LU;
    }

    if (recomputeIntrinsics) {
        uchar* imask = solver.mask->data.ptr + nparams - NINTRINSIC * 2;
        if (!(flags & CALIB_RATIONAL_MODEL))
            flags |= CALIB_FIX_K4 | CALIB_FIX_K5 | CALIB_FIX_K6;
        if (!(flags & CALIB_THIN_PRISM_MODEL))
            flags |= CALIB_FIX_S1_S2_S3_S4;
        if (!(flags & CALIB_TILTED_MODEL))
            flags |= CALIB_FIX_TAUX_TAUY;
        if (flags & CALIB_FIX_ASPECT_RATIO)
            imask[0] = imask[NINTRINSIC] = 0;
        if (flags & CALIB_FIX_FOCAL_LENGTH)
            imask[0] = imask[1] = imask[NINTRINSIC] = imask[NINTRINSIC + 1]
                = 0;
        if (flags & CALIB_FIX_PRINCIPAL_POINT)
            imask[2] = imask[3] = imask[NINTRINSIC + 2]
                = imask[NINTRINSIC + 3] = 0;
        if (flags & (CALIB_ZERO_TANGENT_DIST | CALIB_FIX_TANGENT_DIST))
            imask[6] = imask[7] = imask[NINTRINSIC + 6]
                = imask[NINTRINSIC + 7] = 0;
        if (flags & CALIB_FIX_K1)
            imask[4] = imask[NINTRINSIC + 4] = 0;
        if (flags & CALIB_FIX_K2)
            imask[5] = imask[NINTRINSIC + 5] = 0;
        if (flags & CALIB_FIX_K3)
            imask[8] = imask[NINTRINSIC + 8] = 0;
        if (flags & CALIB_FIX_K4)
            imask[9] = imask[NINTRINSIC + 9] = 0;
        if (flags & CALIB_FIX_K5)
            imask[10] = imask[NINTRINSIC + 10] = 0;
        if (flags & CALIB_FIX_K6)
            imask[11] = imask[NINTRINSIC + 11] = 0;
        if (flags & CALIB_FIX_S1_S2_S3_S4) {
            imask[12] = imask[NINTRINSIC + 12] = 0;
            imask[13] = imask[NINTRINSIC + 13] = 0;
            imask[14] = imask[NINTRINSIC + 14] = 0;
            imask[15] = imask[NINTRINSIC + 15] = 0;
        }
        if (flags & CALIB_FIX_TAUX_TAUY) {
            imask[16] = imask[NINTRINSIC + 16] = 0;
            imask[17] = imask[NINTRINSIC + 17] = 0;
        }
    }

    // storage for initial [om(R){i}|t{i}] (in order to compute the median for
    // each component)
    RT0.reset(cvCreateMat(6, nimages, CV_64F));
    /*
       Compute initial estimate of pose
       For each image, compute:
          R(om) is the rotation matrix of om
          om(R) is the rotation vector of R
          R_ref = R(om_right) * R(om_left)'
          T_ref_list = [T_ref_list; T_right - R_ref * T_left]
          om_ref_list = {om_ref_list; om(R_ref)]
       om = median(om_ref_list)
       T = median(T_ref_list)
    */
    for (i = ofs = 0; i < nimages; ofs += ni, i++) {
        ni = npoints->data.i[i];
        CvMat objpt_i;
        double _om[2][3], r[2][9], t[2][3];
        CvMat om[2], R[2], T[2], imgpt_i[2];

        objpt_i = cvMat(1, ni, CV_64FC3, objectPoints->data.db + ofs * 3);
        for (k = 0; k < 2; k++) {
            imgpt_i[k]
                = cvMat(1, ni, CV_64FC2, imagePoints[k]->data.db + ofs * 2);
            om[k] = cvMat(3, 1, CV_64F, _om[k]);
            R[k] = cvMat(3, 3, CV_64F, r[k]);
            T[k] = cvMat(3, 1, CV_64F, t[k]);

            cvFindExtrinsicCameraParams2(
                &objpt_i, &imgpt_i[k], &K[k], &Dist[k], &om[k], &T[k]);
            cvRodrigues2(&om[k], &R[k]);
            if (k == 0) {
                // save initial om_left and T_left
                solver.param->data.db[(i + 1) * 6] = _om[0][0];
                solver.param->data.db[(i + 1) * 6 + 1] = _om[0][1];
                solver.param->data.db[(i + 1) * 6 + 2] = _om[0][2];
                solver.param->data.db[(i + 1) * 6 + 3] = t[0][0];
                solver.param->data.db[(i + 1) * 6 + 4] = t[0][1];
                solver.param->data.db[(i + 1) * 6 + 5] = t[0][2];
            }
        }
        cvGEMM(&R[1], &R[0], 1, 0, 0, &R[0], CV_GEMM_B_T);
        cvGEMM(&R[0], &T[0], -1, &T[1], 1, &T[1]);
        cvRodrigues2(&R[0], &T[0]);
        RT0->data.db[i] = t[0][0];
        RT0->data.db[i + nimages] = t[0][1];
        RT0->data.db[i + nimages * 2] = t[0][2];
        RT0->data.db[i + nimages * 3] = t[1][0];
        RT0->data.db[i + nimages * 4] = t[1][1];
        RT0->data.db[i + nimages * 5] = t[1][2];
    }

    if (flags & CALIB_USE_EXTRINSIC_GUESS) {
        Vec3d R, T;
        cvarrToMat(matT).convertTo(T, CV_64F);

        if (matR->rows == 3 && matR->cols == 3)
            Rodrigues(cvarrToMat(matR), R);
        else
            cvarrToMat(matR).convertTo(R, CV_64F);

        solver.param->data.db[0] = R[0];
        solver.param->data.db[1] = R[1];
        solver.param->data.db[2] = R[2];
        solver.param->data.db[3] = T[0];
        solver.param->data.db[4] = T[1];
        solver.param->data.db[5] = T[2];
    } else {
        // find the medians and save the first 6 parameters
        for (i = 0; i < 6; i++) {
            qsort(RT0->data.db + i * nimages, nimages,
                CV_ELEM_SIZE(RT0->type), dbCmp);
            solver.param->data.db[i] = nimages % 2 != 0
                ? RT0->data.db[i * nimages + nimages / 2]
                : (RT0->data.db[i * nimages + nimages / 2 - 1]
                      + RT0->data.db[i * nimages + nimages / 2])
                    * 0.5;
        }
    }

    if (recomputeIntrinsics)
        for (k = 0; k < 2; k++) {
            double* iparam
                = solver.param->data.db + (nimages + 1) * 6 + k * NINTRINSIC;
            if (flags & CALIB_ZERO_TANGENT_DIST)
                dk[k][2] = dk[k][3] = 0;
            iparam[0] = A[k][0];
            iparam[1] = A[k][4];
            iparam[2] = A[k][2];
            iparam[3] = A[k][5];
            iparam[4] = dk[k][0];
            iparam[5] = dk[k][1];
            iparam[6] = dk[k][2];
            iparam[7] = dk[k][3];
            iparam[8] = dk[k][4];
            iparam[9] = dk[k][5];
            iparam[10] = dk[k][6];
            iparam[11] = dk[k][7];
            iparam[12] = dk[k][8];
            iparam[13] = dk[k][9];
            iparam[14] = dk[k][10];
            iparam[15] = dk[k][11];
            iparam[16] = dk[k][12];
            iparam[17] = dk[k][13];
        }

    om_LR = cvMat(3, 1, CV_64F, solver.param->data.db);
    T_LR = cvMat(3, 1, CV_64F, solver.param->data.db + 3);

    for (;;) {
        const CvMat* param = 0;
        CvMat *JtJ = 0, *JtErr = 0;
        double* _errNorm = 0;
        double _omR[3], _tR[3];
        double _dr3dr1[9], _dr3dr2[9], /*_dt3dr1[9],*/ _dt3dr2[9], _dt3dt1[9],
            _dt3dt2[9];
        CvMat dr3dr1 = cvMat(3, 3, CV_64F, _dr3dr1);
        CvMat dr3dr2 = cvMat(3, 3, CV_64F, _dr3dr2);
        // CvMat dt3dr1 = cvMat(3, 3, CV_64F, _dt3dr1);
        CvMat dt3dr2 = cvMat(3, 3, CV_64F, _dt3dr2);
        CvMat dt3dt1 = cvMat(3, 3, CV_64F, _dt3dt1);
        CvMat dt3dt2 = cvMat(3, 3, CV_64F, _dt3dt2);
        CvMat om[2], T[2], imgpt_i[2];

        if (!solver.updateAlt(param, JtJ, JtErr, _errNorm))
            break;
        reprojErr = 0;

        cvRodrigues2(&om_LR, &R_LR);
        om[1] = cvMat(3, 1, CV_64F, _omR);
        T[1] = cvMat(3, 1, CV_64F, _tR);

        if (recomputeIntrinsics) {
            double* iparam = solver.param->data.db + (nimages + 1) * 6;
            double* ipparam = solver.prevParam->data.db + (nimages + 1) * 6;

            if (flags & CALIB_SAME_FOCAL_LENGTH) {
                iparam[NINTRINSIC] = iparam[0];
                iparam[NINTRINSIC + 1] = iparam[1];
                ipparam[NINTRINSIC] = ipparam[0];
                ipparam[NINTRINSIC + 1] = ipparam[1];
            }
            if (flags & CALIB_FIX_ASPECT_RATIO) {
                iparam[0] = iparam[1] * aspectRatio[0];
                iparam[NINTRINSIC] = iparam[NINTRINSIC + 1] * aspectRatio[1];
                ipparam[0] = ipparam[1] * aspectRatio[0];
                ipparam[NINTRINSIC]
                    = ipparam[NINTRINSIC + 1] * aspectRatio[1];
            }
            for (k = 0; k < 2; k++) {
                A[k][0] = iparam[k * NINTRINSIC + 0];
                A[k][4] = iparam[k * NINTRINSIC + 1];
                A[k][2] = iparam[k * NINTRINSIC + 2];
                A[k][5] = iparam[k * NINTRINSIC + 3];
                dk[k][0] = iparam[k * NINTRINSIC + 4];
                dk[k][1] = iparam[k * NINTRINSIC + 5];
                dk[k][2] = iparam[k * NINTRINSIC + 6];
                dk[k][3] = iparam[k * NINTRINSIC + 7];
                dk[k][4] = iparam[k * NINTRINSIC + 8];
                dk[k][5] = iparam[k * NINTRINSIC + 9];
                dk[k][6] = iparam[k * NINTRINSIC + 10];
                dk[k][7] = iparam[k * NINTRINSIC + 11];
                dk[k][8] = iparam[k * NINTRINSIC + 12];
                dk[k][9] = iparam[k * NINTRINSIC + 13];
                dk[k][10] = iparam[k * NINTRINSIC + 14];
                dk[k][11] = iparam[k * NINTRINSIC + 15];
                dk[k][12] = iparam[k * NINTRINSIC + 16];
                dk[k][13] = iparam[k * NINTRINSIC + 17];
            }
        }

        for (i = ofs = 0; i < nimages; ofs += ni, i++) {
            ni = npoints->data.i[i];
            CvMat objpt_i;

            om[0] = cvMat(3, 1, CV_64F, solver.param->data.db + (i + 1) * 6);
            T[0] = cvMat(
                3, 1, CV_64F, solver.param->data.db + (i + 1) * 6 + 3);

            if (JtJ || JtErr)
                cvComposeRT(&om[0], &T[0], &om_LR, &T_LR, &om[1], &T[1],
                    &dr3dr1, 0, &dr3dr2, 0, 0, &dt3dt1, &dt3dr2, &dt3dt2);
            else
                cvComposeRT(&om[0], &T[0], &om_LR, &T_LR, &om[1], &T[1]);

            objpt_i = cvMat(1, ni, CV_64FC3, objectPoints->data.db + ofs * 3);
            err.resize(ni * 2);
            Je.resize(ni * 2);
            J_LR.resize(ni * 2);
            Ji.resize(ni * 2);

            CvMat tmpimagePoints = cvMat(err.reshape(2, 1));
            CvMat dpdf = cvMat(Ji.colRange(0, 2));
            CvMat dpdc = cvMat(Ji.colRange(2, 4));
            CvMat dpdk = cvMat(Ji.colRange(4, NINTRINSIC));
            CvMat dpdrot = cvMat(Je.colRange(0, 3));
            CvMat dpdt = cvMat(Je.colRange(3, 6));

            for (k = 0; k < 2; k++) {
                imgpt_i[k] = cvMat(
                    1, ni, CV_64FC2, imagePoints[k]->data.db + ofs * 2);

                if (JtJ || JtErr)
                    cvProjectPoints2(&objpt_i, &om[k], &T[k], &K[k], &Dist[k],
                        &tmpimagePoints, &dpdrot, &dpdt, &dpdf, &dpdc, &dpdk,
                        (flags & CALIB_FIX_ASPECT_RATIO) ? aspectRatio[k]
                                                         : 0);
                else
                    cvProjectPoints2(&objpt_i, &om[k], &T[k], &K[k], &Dist[k],
                        &tmpimagePoints);
                cvSub(&tmpimagePoints, &imgpt_i[k], &tmpimagePoints);

                if (solver.state == CvLevMarq::CALC_J) {
                    int iofs = (nimages + 1) * 6 + k * NINTRINSIC,
                        eofs = (i + 1) * 6;
                    assert(JtJ && JtErr);

                    Mat _JtJ(cvarrToMat(JtJ)), _JtErr(cvarrToMat(JtErr));

                    if (k == 1) {
                        // d(err_{x|y}R) ~ de3
                        // convert de3/{dr3,dt3} => de3{dr1,dt1} &
                        // de3{dr2,dt2}
                        for (p = 0; p < ni * 2; p++) {
                            CvMat de3dr3 = cvMat(1, 3, CV_64F, Je.ptr(p));
                            CvMat de3dt3
                                = cvMat(1, 3, CV_64F, de3dr3.data.db + 3);
                            CvMat de3dr2 = cvMat(1, 3, CV_64F, J_LR.ptr(p));
                            CvMat de3dt2
                                = cvMat(1, 3, CV_64F, de3dr2.data.db + 3);
                            double _de3dr1[3], _de3dt1[3];
                            CvMat de3dr1 = cvMat(1, 3, CV_64F, _de3dr1);
                            CvMat de3dt1 = cvMat(1, 3, CV_64F, _de3dt1);

                            cvMatMul(&de3dr3, &dr3dr1, &de3dr1);
                            cvMatMul(&de3dt3, &dt3dt1, &de3dt1);

                            cvMatMul(&de3dr3, &dr3dr2, &de3dr2);
                            cvMatMulAdd(&de3dt3, &dt3dr2, &de3dr2, &de3dr2);

                            cvMatMul(&de3dt3, &dt3dt2, &de3dt2);

                            cvCopy(&de3dr1, &de3dr3);
                            cvCopy(&de3dt1, &de3dt3);
                        }

                        _JtJ(Rect(0, 0, 6, 6)) += J_LR.t() * J_LR;
                        _JtJ(Rect(eofs, 0, 6, 6)) = J_LR.t() * Je;
                        _JtErr.rowRange(0, 6) += J_LR.t() * err;
                    }

                    _JtJ(Rect(eofs, eofs, 6, 6)) += Je.t() * Je;
                    _JtErr.rowRange(eofs, eofs + 6) += Je.t() * err;

                    if (recomputeIntrinsics) {
                        _JtJ(Rect(iofs, iofs, NINTRINSIC, NINTRINSIC))
                            += Ji.t() * Ji;
                        _JtJ(Rect(iofs, eofs, NINTRINSIC, 6)) += Je.t() * Ji;
                        if (k == 1) {
                            _JtJ(Rect(iofs, 0, NINTRINSIC, 6))
                                += J_LR.t() * Ji;
                        }
                        _JtErr.rowRange(iofs, iofs + NINTRINSIC)
                            += Ji.t() * err;
                    }
                }

                double viewErr = norm(err, NORM_L2SQR);

                if (perViewErr)
                    perViewErr->data.db[i * 2 + k] = std::sqrt(viewErr / ni);

                reprojErr += viewErr;
            }
        }
        if (_errNorm)
            *_errNorm = reprojErr;
    }

    cvRodrigues2(&om_LR, &R_LR);
    if (matR->rows == 1 || matR->cols == 1)
        cvConvert(&om_LR, matR);
    else
        cvConvert(&R_LR, matR);
    cvConvert(&T_LR, matT);

    if (recomputeIntrinsics) {
        cvConvert(&K[0], _cameraMatrix1);
        cvConvert(&K[1], _cameraMatrix2);

        for (k = 0; k < 2; k++) {
            CvMat* distCoeffs = k == 0 ? _distCoeffs1 : _distCoeffs2;
            CvMat tdist = cvMat(distCoeffs->rows, distCoeffs->cols,
                CV_MAKETYPE(CV_64F, CV_MAT_CN(distCoeffs->type)),
                Dist[k].data.db);
            cvConvert(&tdist, distCoeffs);
        }
    }

    if (matE || matF) {
        double* t = T_LR.data.db;
        double tx[] = { 0, -t[2], t[1], t[2], 0, -t[0], -t[1], t[0], 0 };
        CvMat Tx = cvMat(3, 3, CV_64F, tx);
        double e[9], f[9];
        CvMat E = cvMat(3, 3, CV_64F, e);
        CvMat F = cvMat(3, 3, CV_64F, f);
        cvMatMul(&Tx, &R_LR, &E);
        if (matE)
            cvConvert(&E, matE);
        if (matF) {
            double ik[9];
            CvMat iK = cvMat(3, 3, CV_64F, ik);
            cvInvert(&K[1], &iK);
            cvGEMM(&iK, &E, 1, 0, 0, &E, CV_GEMM_A_T);
            cvInvert(&K[0], &iK);
            cvMatMul(&E, &iK, &F);
            cvConvertScale(&F, matF, fabs(f[8]) > 0 ? 1. / f[8] : 1);
        }
    }

    return std::sqrt(reprojErr / (pointsTotal * 2));
}

double stereoCalibrate(InputArrayOfArrays _objectPoints,
    InputArrayOfArrays _imagePoints1, InputArrayOfArrays _imagePoints2,
    InputOutputArray _cameraMatrix1, InputOutputArray _distCoeffs1,
    InputOutputArray _cameraMatrix2, InputOutputArray _distCoeffs2,
    Size imageSize, InputOutputArray _Rmat, InputOutputArray _Tmat,
    OutputArray _Emat, OutputArray _Fmat, OutputArray _perViewErrors,
    int flags, TermCriteria criteria)
{
    int rtype = CV_64F;
    Mat cameraMatrix1 = _cameraMatrix1.getMat();
    Mat cameraMatrix2 = _cameraMatrix2.getMat();
    Mat distCoeffs1 = _distCoeffs1.getMat();
    Mat distCoeffs2 = _distCoeffs2.getMat();
    cameraMatrix1 = prepareCameraMatrix(cameraMatrix1, rtype);
    cameraMatrix2 = prepareCameraMatrix(cameraMatrix2, rtype);
    distCoeffs1 = prepareDistCoeffs(distCoeffs1, rtype);
    distCoeffs2 = prepareDistCoeffs(distCoeffs2, rtype);

    if (!(flags & CALIB_RATIONAL_MODEL) && (!(flags & CALIB_THIN_PRISM_MODEL))
        && (!(flags & CALIB_TILTED_MODEL))) {
        distCoeffs1 = distCoeffs1.rows == 1 ? distCoeffs1.colRange(0, 5)
                                            : distCoeffs1.rowRange(0, 5);
        distCoeffs2 = distCoeffs2.rows == 1 ? distCoeffs2.colRange(0, 5)
                                            : distCoeffs2.rowRange(0, 5);
    }

    if ((flags & CALIB_USE_EXTRINSIC_GUESS) == 0) {
        _Rmat.create(3, 3, rtype);
        _Tmat.create(3, 1, rtype);
    }

    Mat objPt, imgPt, imgPt2, npoints;

    collectCalibrationData(_objectPoints, _imagePoints1, _imagePoints2, objPt,
        imgPt, &imgPt2, npoints);
    CvMat c_objPt = cvMat(objPt), c_imgPt = cvMat(imgPt),
          c_imgPt2 = cvMat(imgPt2), c_npoints = cvMat(npoints);
    CvMat c_cameraMatrix1 = cvMat(cameraMatrix1),
          c_distCoeffs1 = cvMat(distCoeffs1);
    CvMat c_cameraMatrix2 = cvMat(cameraMatrix2),
          c_distCoeffs2 = cvMat(distCoeffs2);
    Mat matR_ = _Rmat.getMat(), matT_ = _Tmat.getMat();
    CvMat c_matR = cvMat(matR_), c_matT = cvMat(matT_), c_matE, c_matF,
          c_matErr;

    bool E_needed = _Emat.needed(), F_needed = _Fmat.needed(),
         errors_needed = _perViewErrors.needed();

    Mat matE_, matF_, matErr_;
    if (E_needed) {
        _Emat.create(3, 3, rtype);
        matE_ = _Emat.getMat();
        c_matE = cvMat(matE_);
    }
    if (F_needed) {
        _Fmat.create(3, 3, rtype);
        matF_ = _Fmat.getMat();
        c_matF = cvMat(matF_);
    }

    if (errors_needed) {
        int nimages = int(_objectPoints.total());
        _perViewErrors.create(nimages, 2, CV_64F);
        matErr_ = _perViewErrors.getMat();
        c_matErr = cvMat(matErr_);
    }

    double err = stereoCalibrateImpl(&c_objPt, &c_imgPt, &c_imgPt2,
        &c_npoints, &c_cameraMatrix1, &c_distCoeffs1, &c_cameraMatrix2,
        &c_distCoeffs2, cvSize(imageSize), &c_matR, &c_matT,
        E_needed ? &c_matE : NULL, F_needed ? &c_matF : NULL,
        errors_needed ? &c_matErr : NULL, flags, cvTermCriteria(criteria));

    cameraMatrix1.copyTo(_cameraMatrix1);
    cameraMatrix2.copyTo(_cameraMatrix2);
    distCoeffs1.copyTo(_distCoeffs1);
    distCoeffs2.copyTo(_distCoeffs2);

    return err;
}

double stereoCalibrate(InputArrayOfArrays _objectPoints,
    InputArrayOfArrays _imagePoints1, InputArrayOfArrays _imagePoints2,
    InputOutputArray _cameraMatrix1, InputOutputArray _distCoeffs1,
    InputOutputArray _cameraMatrix2, InputOutputArray _distCoeffs2,
    Size imageSize, OutputArray _Rmat, OutputArray _Tmat, OutputArray _Emat,
    OutputArray _Fmat, int flags, TermCriteria criteria)
{
    Mat Rmat, Tmat;
    double ret
        = calrel::stereoCalibrate(_objectPoints, _imagePoints1, _imagePoints2,
            _cameraMatrix1, _distCoeffs1, _cameraMatrix2, _distCoeffs2,
            imageSize, Rmat, Tmat, _Emat, _Fmat, noArray(), flags, criteria);
    Rmat.copyTo(_Rmat);
    Tmat.copyTo(_Tmat);
    return ret;
}

} /* end of namespace calrel */
