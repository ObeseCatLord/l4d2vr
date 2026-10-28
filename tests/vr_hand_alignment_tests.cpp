#include <limits>
#if defined(VECTOR_PARANOIA) && !defined(VEC_T_NAN)
#define VEC_T_NAN std::numeric_limits<float>::quiet_NaN()
#endif
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "../L4D2VR/vr_hands/vr_hand_math.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <cstdio>
#include <cstdlib>
#include "../L4D2VR/vr_ammo_grip.h"
static void CheckImpl(bool x, int line) { if (!x) { std::fprintf(stderr,"Hand anchor regression failed at line %d\n",line); std::exit(1); } }
#define Check(value) CheckImpl((value), __LINE__)
static bool Near(const Vector& a, const Vector& b) { return (a-b).LengthSqr() < 0.00001f; }
static Vector Point(const VrHandMatrix4& matrix, const Vector& point)
{
    return l4d2vr_ammo_grip::Origin(matrix) + l4d2vr_ammo_grip::Rotate(matrix, point);
}
int main()
{
    const Vector p(30.0f,50.0f,80.0f), offset(0.0f,-0.08f,0.1f), rotation(0.0f,180.0f,0.0f);
    const QAngle angle(20.0f,75.0f,40.0f);
    const float units=43.2f;
    const Vector zero(0.0f,0.0f,0.0f);
    const auto rendered=VrHandMath::BuildControllerWorld(p,angle,units,1.2f,offset,rotation);
    const auto anchor=VrHandMath::BuildControllerWorld(p,angle,units,1.0f/units,offset,rotation);
    for (int row=0;row<3;++row)
    {
        Check(std::fabs(VrHandMath::Get(rendered,row,3)-VrHandMath::Get(anchor,row,3))<0.0001f);
        for (int col=0;col<3;++col)
            Check(std::fabs(VrHandMath::Get(rendered,row,col)/(units*1.2f)-VrHandMath::Get(anchor,row,col))<0.0001f);
    }
    // The same local palm point must land at the same world position in the
    // scaled glove mesh and in the unscaled interaction anchor.
    for (int row=0;row<3;++row)
        Check(std::fabs(VrHandMath::Get(rendered,row,3)+VrHandMath::Get(rendered,row,2)*0.07f/1.2f-
            VrHandMath::Get(anchor,row,3)-VrHandMath::Get(anchor,row,2)*0.07f*units)<0.0001f);

    // Acquire ammunition at different wrist angles and body heights. Its
    // model grip point must stay at the palm, with the same local orientation.
    const Vector boxCenter(2.0f, -1.0f, 4.0f), gripPoint(2.0f, -1.0f, 1.0f);
    const Vector palmLocal(0.4f, -0.2f, 3.0f), correction(15.0f, 90.0f, -20.0f);
    l4d2vr_ammo_grip::Pose firstGrip{};
    for (int i=0;i<12;++i)
    {
        const float step=static_cast<float>(i);
        const auto controller=VrHandMath::BuildControllerWorld(Vector(step*6.0f, 20.0f, 90.0f-step*2.0f),
            QAngle(step*7.0f, step*30.0f, step*-11.0f), units, 1.0f/units, zero, zero);
        const Vector palm=Point(controller,palmLocal);
        l4d2vr_ammo_grip::Pose grip{};
        Check(l4d2vr_ammo_grip::Capture(controller,correction,palm,boxCenter,gripPoint,grip));
        if (i==0) firstGrip=grip;
        Check(Near(firstGrip.centerOffsetLocal,grip.centerOffsetLocal));
        for (int col=0;col<3;++col)
            Check(Near(l4d2vr_ammo_grip::Axis(firstGrip.orientationLocal,col),
                l4d2vr_ammo_grip::Axis(grip.orientationLocal,col)));
        VrHandMatrix4 held{};
        Check(l4d2vr_ammo_grip::Follow(controller,grip,boxCenter,held));
        Check(Near(Point(held,gripPoint),palm) && l4d2vr_ammo_grip::Rigid(held));

        // A later render pose uses the same stored grip. Translate and rotate
        // the controller while retaining the physical point inside the palm.
        const auto next=VrHandMath::BuildControllerWorld(Vector(-30.0f+step, 50.0f, 70.0f),
            QAngle(-20.0f, step*23.0f, 37.0f), units, 1.0f/units, zero, zero);
        Check(l4d2vr_ammo_grip::Follow(next,grip,boxCenter,held));
        Check(Near(Point(held,gripPoint),Point(next,palmLocal)));
        // Glove mode grips at the center rather than the bottom of the magazine.
        Check(l4d2vr_ammo_grip::Capture(controller,correction,palm,boxCenter,boxCenter,grip));
        Check(l4d2vr_ammo_grip::Follow(next,grip,boxCenter,held));
        Check(Near(Point(held,boxCenter),Point(next,palmLocal)));
    }
    // Shell-sized and replacement-model bounds use their own local grip point,
    // rather than inheriting the center offset of the previously held magazine.
    const auto controller=VrHandMath::BuildControllerWorld(p,angle,units,1.0f/units,zero,zero);
    l4d2vr_ammo_grip::Pose shell{};
    Check(l4d2vr_ammo_grip::Capture(controller,zero,Point(controller,palmLocal),zero,zero,shell));
    VrHandMatrix4 held{};
    Check(l4d2vr_ammo_grip::Follow(controller,shell,zero,held));
    Check(Near(l4d2vr_ammo_grip::Origin(held),Point(controller,palmLocal)));
    const auto saved=shell;
    const float nan=std::numeric_limits<float>::quiet_NaN();
    Check(!l4d2vr_ammo_grip::Capture(controller,Vector(nan,0.0f,0.0f),p,boxCenter,gripPoint,shell));
    Check(Near(shell.centerOffsetLocal,saved.centerOffsetLocal));
    auto invalid=controller;
    VrHandMath::Set(invalid,0,0,VrHandMath::Get(invalid,0,0)*2.0f);
    Check(!l4d2vr_ammo_grip::Capture(invalid,correction,p,boxCenter,gripPoint,shell));
    invalid=controller;
    for (int row=0;row<3;++row) VrHandMath::Set(invalid,row,0,-VrHandMath::Get(invalid,row,0));
    Check(!l4d2vr_ammo_grip::Follow(invalid,saved,boxCenter,held)); // do not mirror the ammo mesh
    Check(!l4d2vr_ammo_grip::Follow(controller,saved,Vector(0.0f,nan,0.0f),held));
    const auto defaultHand=VrHandMath::BuildControllerWorld(p,angle,units);
    const auto explicitHand=VrHandMath::BuildControllerWorld(p,angle,units,1.0f,zero,zero);
    for (int i=0;i<16;++i) Check(defaultHand.m[static_cast<size_t>(i)]==explicitHand.m[static_cast<size_t>(i)]);
    // Waist previews and waiting ammo use the same body frame, with outward
    // winding preserved. Gun orientation is not an input to this transform.
    for (int i=0;i<8;++i)
    {
        Vector forward, right, up;
        QAngle::AngleVectors(QAngle(0.0f,static_cast<float>(i)*45.0f,0.0f),&forward,&right,&up);
        const Vector waist=p+forward*4.0f+right*-9.0f-Vector(0.0f,0.0f,25.0f);
        Check(l4d2vr_ammo_grip::BodyWorld(forward,right,waist,boxCenter,held));
        Check(Near(Point(held,boxCenter),waist) && l4d2vr_ammo_grip::Rigid(held));
        Check(Near(l4d2vr_ammo_grip::Axis(held,0),forward));
        Check(Near(l4d2vr_ammo_grip::Axis(held,1),right*-1.0f));
        VrHandMatrix4 translated{};
        const Vector motion(10.0f,-30.0f,5.0f);
        Check(l4d2vr_ammo_grip::BodyWorld(forward,right,waist+motion,boxCenter,translated));
        Check(Near(l4d2vr_ammo_grip::Origin(translated)-l4d2vr_ammo_grip::Origin(held),motion));
    }
    Check(!l4d2vr_ammo_grip::BodyWorld(Vector(0.0f,0.0f,1.0f),Vector(0.0f,-1.0f,0.0f),p,boxCenter,held));

    // Natural hold: with the off hand turned as the gun hand and raised by the
    // tilt, the spare sits exactly as the magazine sits in the well, whatever
    // the gun's pose and the magazine bone's axes.
    auto sameRotation=[](const VrHandMatrix4& a,const VrHandMatrix4& b)
    {
        for (int col=0;col<3;++col) if (!Near(l4d2vr_ammo_grip::Axis(a,col),l4d2vr_ammo_grip::Axis(b,col))) return false;
        return true;
    };
    const Vector boneTurns[]={Vector(0.0f,0.0f,0.0f),Vector(-90.0f,0.0f,90.0f),Vector(30.0f,170.0f,-20.0f),Vector(0.0f,180.0f,0.0f)};
    for (int i=0;i<8;++i)
    {
        const float step=static_cast<float>(i);
        const auto gun=VrHandMath::BuildControllerWorld(Vector(step*5.0f,-10.0f,60.0f),
            QAngle(-30.0f+step*9.0f,step*47.0f,step*13.0f),units,1.0f/units,zero,zero);
        for (const Vector& turn : boneTurns)
        {
            auto well=VrHandMath::Multiply(gun,l4d2vr_ammo_grip::Calibration(turn));
            VrHandMath::Set(well,0,3,VrHandMath::Get(well,0,3)+12.0f); // the well is ahead of the hand
            for (float tilt : {0.0f,5.0f,15.0f})
            {
                VrHandMatrix4 hold{};
                Check(l4d2vr_ammo_grip::NaturalHold(gun,well,tilt,zero,hold) && l4d2vr_ammo_grip::Rigid(hold));
                const auto natural=VrHandMath::Multiply(gun,l4d2vr_ammo_grip::Calibration(Vector(tilt,0.0f,0.0f)));
                const auto spare=VrHandMath::Multiply(natural,hold);
                Check(sameRotation(spare,well));
                Check(l4d2vr_ammo_grip::AngleBetweenDeg(spare,well)<0.1f);
                // Raising the hand nose-up really points it up: the forward
                // axis (-z) gains height toward the gun's up.
                Check(VrHandMath::Dot(l4d2vr_ammo_grip::Axis(natural,2)*-1.0f,l4d2vr_ammo_grip::Axis(gun,1))>=
                    std::sin(tilt*3.14159265f/180.0f)-0.001f);
                // Held parallel to the gun hand the spare leans by the tilt.
                Check(std::fabs(l4d2vr_ammo_grip::AngleBetweenDeg(VrHandMath::Multiply(gun,hold),well)-tilt)<0.1f);
                // The configured rotation turns it further in the hand.
                VrHandMatrix4 turned{};
                const Vector extra(10.0f,-20.0f,5.0f);
                Check(l4d2vr_ammo_grip::NaturalHold(gun,well,tilt,extra,turned));
                Check(sameRotation(turned,VrHandMath::Multiply(l4d2vr_ammo_grip::Calibration(extra),hold)));
                // The Calibration angles read back.
                Check(sameRotation(l4d2vr_ammo_grip::Calibration(l4d2vr_ammo_grip::CalibrationDegrees(hold)),hold));
            }
        }
    }
    {
        const auto gun=VrHandMath::BuildControllerWorld(p,angle,units,1.0f/units,zero,zero);
        auto mirrored=gun;
        for (int row=0;row<3;++row) VrHandMath::Set(mirrored,row,0,-VrHandMath::Get(mirrored,row,0));
        VrHandMatrix4 hold{};
        Check(!l4d2vr_ammo_grip::NaturalHold(gun,mirrored,15.0f,zero,hold));
        Check(!l4d2vr_ammo_grip::NaturalHold(gun,gun,std::numeric_limits<float>::quiet_NaN(),zero,hold));
        // Capture with a hold matrix keeps it.
        l4d2vr_ammo_grip::Pose pose{};
        const auto turn=l4d2vr_ammo_grip::Calibration(Vector(20.0f,-50.0f,70.0f));
        Check(l4d2vr_ammo_grip::Capture(gun,turn,Point(gun,palmLocal),boxCenter,gripPoint,pose));
        Check(sameRotation(pose.orientationLocal,turn));
        // The well accepts a spare only facing the way it goes in.
        const float minDot=std::cos(45.0f*3.14159265f/180.0f);
        Check(l4d2vr_ammo_grip::AxesAgree(gun,gun,minDot));
        Check(l4d2vr_ammo_grip::AxesAgree(VrHandMath::Multiply(gun,l4d2vr_ammo_grip::Calibration(Vector(30.0f,0.0f,0.0f))),gun,minDot));
        Check(!l4d2vr_ammo_grip::AxesAgree(VrHandMath::Multiply(gun,l4d2vr_ammo_grip::Calibration(Vector(60.0f,0.0f,0.0f))),gun,minDot));
        Check(!l4d2vr_ammo_grip::AxesAgree(VrHandMath::Multiply(gun,l4d2vr_ammo_grip::Calibration(Vector(0.0f,180.0f,0.0f))),gun,minDot)); // backwards
        Check(!l4d2vr_ammo_grip::AxesAgree(VrHandMath::Multiply(gun,l4d2vr_ammo_grip::Calibration(Vector(180.0f,0.0f,0.0f))),gun,minDot)); // upside down
    }
}
