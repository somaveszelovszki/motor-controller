#include <micro/hw/DC_Motor.hpp>
#include <micro/hw/Encoder.hpp>
#include <micro/hw/SteeringServo.hpp>
#include <micro/control/PID_Controller.hpp>
#include <micro/panel/CanManager.hpp>
#include <micro/port/task.hpp>
#include <micro/utils/timer.hpp>
#include <micro/utils/CarProps.hpp>

#include <cfg_board.h>
#include <cfg_car.hpp>
#include <LateralControl.hpp>
#include <LongitudinalControl.hpp>
#include <RemoteControllerData.hpp>

#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

using namespace micro;

extern queue_t<RemoteControllerData, 1> remoteControllerQueue;

CanManager vehicleCanManager(can_Vehicle, canRxFifo_Vehicle, millisecond_t(50));

namespace {

bool useSafetyEnableSignal         = true;
micro::CarProps car                = micro::CarProps();
micro::radian_t frontWheelOffset   = micro::radian_t(0);
micro::radian_t frontWheelMaxDelta = micro::radian_t(0);
micro::radian_t rearWheelOffset    = micro::radian_t(0);
micro::radian_t rearWheelMaxDelta  = micro::radian_t(0);
micro::radian_t extraServoOffset   = micro::radian_t(0);
micro::radian_t extraServoMaxDelta = micro::radian_t(0);

float motorCtrl_P = 0.0f; // TODO
float motorCtrl_I = 0.0f;

hw::DC_Motor dcMotor(tim_DC_Motor, timChnl_DC_Motor_Bridge1, timChnl_DC_Motor_Bridge2, cfg::MOTOR_MAX_DUTY);
hw::Encoder encoder(tim_Encoder);
PID_Controller speedCtrl({ motorCtrl_P, motorCtrl_I, 0.0f }, cfg::MOTOR_MAX_DUTY, 0.01f);

hw::SteeringServo frontSteeringServo(tim_SteeringServo, timChnl_FrontSteeringServo, cfg::FRONT_STEERING_SERVO_PWM0, cfg::FRONT_STEERING_SERVO_PWM180,
    cfg::SERVO_MAX_ANGULAR_VELO, frontWheelOffset, frontWheelMaxDelta, cfg::SERVO_WHEEL_TRANSFER_RATE);

hw::SteeringServo rearSteeringServo(tim_SteeringServo, timChnl_RearSteeringServo, cfg::REAR_STEERING_SERVO_PWM0, cfg::REAR_STEERING_SERVO_PWM180,
    cfg::SERVO_MAX_ANGULAR_VELO, rearWheelOffset, rearWheelMaxDelta, cfg::SERVO_WHEEL_TRANSFER_RATE);

} // namespace

extern "C" void runControlTask(void) {

    RemoteControllerData remoteControllerData;

    struct {
        micro::radian_t frontWheelAngle;
        micro::radian_t rearWheelAngle;
        micro::radian_t extraServoAngle;
    } lateralControl;

    struct {
        micro::m_per_sec_t speed;
        micro::millisecond_t rampTime;
    } longitudinalControl;

    struct {
        m_per_sec_t startSpeed, targetSpeed;
        microsecond_t startTime, duration;
    } speedRamp;

    canFrame_t rxCanFrame;
    CanFrameHandler vehicleCanFrameHandler;

    vehicleCanFrameHandler.registerHandler(can::LateralControl::id(), [&lateralControl] (const uint8_t * const data) {
        reinterpret_cast<const can::LateralControl*>(data)->acquire(lateralControl.frontWheelAngle, lateralControl.rearWheelAngle, lateralControl.extraServoAngle);
    });

    vehicleCanFrameHandler.registerHandler(can::LongitudinalControl::id(), [&longitudinalControl] (const uint8_t * const data) {
        LongitudinalControl longitudinal;
        reinterpret_cast<const can::LongitudinalControl*>(data)->acquire(longitudinalControl.speed, useSafetyEnableSignal, longitudinalControl.rampTime);
    });

    vehicleCanFrameHandler.registerHandler(can::SetMotorControlParams::id(), [] (const uint8_t * const data) {
        reinterpret_cast<const can::SetMotorControlParams*>(data)->acquire(motorCtrl_P, motorCtrl_I);
    });

    vehicleCanFrameHandler.registerHandler(can::SetFrontWheelParams::id(), [] (const uint8_t * const data) {
        reinterpret_cast<const can::SetFrontWheelParams*>(data)->acquire(frontWheelOffset, frontWheelMaxDelta);
        vehicleCanManager.send(can::FrontWheelParams(frontWheelOffset, frontWheelMaxDelta));
    });

    vehicleCanFrameHandler.registerHandler(can::SetRearWheelParams::id(), [] (const uint8_t * const data) {
        reinterpret_cast<const can::SetRearWheelParams*>(data)->acquire(rearWheelOffset, rearWheelMaxDelta);
        vehicleCanManager.send(can::RearWheelParams(rearWheelOffset, rearWheelMaxDelta));
    });

    const CanManager::subscriberId_t vehicleCanSubsciberId = vehicleCanManager.registerSubscriber(vehicleCanFrameHandler.identifiers());

    WatchdogTimer remoteControlWd(millisecond_t(50));

    while (true) {
        bool isOk = !vehicleCanManager.hasRxTimedOut() && !remoteControlWd.hasTimedOut(); // TODO framework-wide solution for task isOk

        if (vehicleCanManager.read(vehicleCanSubsciberId, rxCanFrame)) {
            vehicleCanFrameHandler.handleFrame(rxCanFrame);
        }

        if (remoteControllerQueue.receive(remoteControllerData, millisecond_t(0))) {
            remoteControlWd.reset();
        }

        frontSteeringServo.setWheelOffset(frontWheelOffset);
        frontSteeringServo.setWheelMaxDelta(frontWheelMaxDelta);
        rearSteeringServo.setWheelOffset(rearWheelOffset);
        rearSteeringServo.setWheelMaxDelta(rearWheelMaxDelta);

        switch (remoteControllerData.activeChannel) {

        case RemoteControllerData::channel_t::DirectControl:
            speedRamp.startSpeed = speedRamp.targetSpeed = map(remoteControllerData.acceleration, -1.0f, 1.0f, -cfg::DIRECT_CONTROL_MAX_SPEED, cfg::DIRECT_CONTROL_MAX_SPEED);
            speedRamp.startTime  = getExactTime();
            speedRamp.duration   = millisecond_t(0);

            frontSteeringServo.writeWheelAngle(map(remoteControllerData.steering, -1.0f, 1.0f, -frontSteeringServo.wheelMaxDelta(), frontSteeringServo.wheelMaxDelta()));
            rearSteeringServo.writeWheelAngle(map(remoteControllerData.steering, -1.0f, 1.0f, rearSteeringServo.wheelMaxDelta(), -rearSteeringServo.wheelMaxDelta()));
            break;

        case RemoteControllerData::channel_t::SafetyEnable:
            if (isOk && isBtw(remoteControllerData.acceleration, 0.5f, 1.0f)) {
                if (longitudinalControl.speed != speedRamp.targetSpeed || longitudinalControl.rampTime != speedRamp.duration) {
                    speedRamp.startSpeed  = car.speed;
                    speedRamp.targetSpeed = longitudinalControl.speed;
                    speedRamp.startTime   = getExactTime();
                    speedRamp.duration    = longitudinalControl.rampTime;
                }

                frontSteeringServo.writeWheelAngle(lateralControl.frontWheelAngle);
                rearSteeringServo.writeWheelAngle(lateralControl.rearWheelAngle);
            } else {
                if (speedRamp.targetSpeed != m_per_sec_t(0) || speedRamp.duration != cfg::EMERGENCY_BRAKE_DURATION) {
                    speedRamp.startSpeed  = car.speed;
                    speedRamp.targetSpeed = m_per_sec_t(0);
                    speedRamp.startTime   = getExactTime();
                    speedRamp.duration    = cfg::EMERGENCY_BRAKE_DURATION;
                }

                frontSteeringServo.writeWheelAngle(radian_t(0));
                rearSteeringServo.writeWheelAngle(radian_t(0));
            }
            break;

        default:
            break;
        }

        speedCtrl.desired = map(getExactTime(), speedRamp.startTime, speedRamp.startTime + speedRamp.duration, speedRamp.startSpeed, speedRamp.targetSpeed).get();

        car.frontWheelAngle = frontSteeringServo.wheelAngle();
        car.rearWheelAngle  = rearSteeringServo.wheelAngle();

        os_delay(1);
    }
}

void tim_ControlLoop_PeriodElapsedCallback() {

    static millisecond_t lastUpdateTime = getExactTime();

    const millisecond_t now = getExactTime();
    encoder.update();

    car.speed = encoder.lastDiff() * cfg::ENCODER_INCR_DISTANCE / (now - lastUpdateTime);
    car.distance = encoder.numIncr() * cfg::ENCODER_INCR_DISTANCE;

    speedCtrl.update(car.speed.get());
    dcMotor.write(speedCtrl.output());

    lastUpdateTime = now;
}

void micro_Vehicle_Can_RxFifoMsgPendingCallback() {
    vehicleCanManager.onFrameReceived();
}
