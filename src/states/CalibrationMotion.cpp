#include "CalibrationMotion.h"
#include "../ForceSensorCalibration.h"
#include <mc_filter/utils/clamp.h>


void CalibrationMotion::start(mc_control::fsm::Controller & ctl)
{
  auto & robot = ctl.robot();
  auto robotConf = ctl.config()(robot.name());
  if(!robotConf.has("motion"))
  {
    LOG_ERROR("Calibration controller expects a joints entry");
    output("FAILURE");
  }
  auto conf = robotConf("motion");
  conf("duration", duration_);
  conf("percentLimits", percentLimits_);
  mc_filter::utils::clampInPlace(percentLimits_, 0, 1);

  auto postureTask = ctl.getPostureTask(robot.name());
  savedStiffness_ = postureTask->stiffness();
  postureTask->stiffness(conf("stiffness", 10));
  constexpr double PI = mc_rtc::constants::PI;
  for(const auto & jConfig : conf("joints"))
  {
    std::string name = jConfig("name");
    auto percentLimits = percentLimits_;
    jConfig("percentLimits", percentLimits);
    mc_filter::utils::clampInPlace(percentLimits, 0, 1);
    double period = jConfig("period");
    auto jidx = robot.jointIndexByName(name);
    auto start = robot.mbc().q[jidx][0];
    auto actualLower = robot.ql()[jidx][0];
    auto actualUpper = robot.qu()[jidx][0];
    auto actualRange = actualUpper - actualLower;

    // Reduced range
    const auto range = percentLimits * actualRange;
    const auto lower = actualLower + (actualRange - range)/2;
    const auto upper = actualUpper - (actualRange - range)/2;

    if(start < lower || start > upper)
    {
      LOG_ERROR("[ForceSensorCalibration] Starting joint configuration of joint " << name << " [" << start << "] is outside of the reduced limit range [" << lower << ", " << upper << "] (percentLimits: " << percentLimits << ", actual joint limits: [" << actualLower << ", " << actualUpper << "]");
      output("FAILURE");
    }

    // compute the starting time such that the joint does not move initially
    // that is such that f(start_dt) = start
    // i.e start_dt = f^(-1)(start)
    double start_dt = period * (acos(sqrt(start - lower)/sqrt(upper-lower))) / PI;
    jointUpdates_.emplace_back(
      /* f(t): periodic function that moves the joint between its limits */
      [this, postureTask, PI, start, lower, upper, start_dt, period, name]()
      {
        auto t = start_dt + dt_;
        auto q = lower + (upper-lower) * (1 + cos((2*PI*t)/period)) / 2;
        postureTask->target({{name, {q}}});
      }
    );
  }

  ctl.gui()->addElement({},
                         mc_rtc::gui::NumberSlider("Progress",
                                                   [this]()
                                                   {
                                                    return dt_;
                                                   },
                                                   [](double)
                                                   {
                                                   },
                                                   0,
                                                   duration_),
                         mc_rtc::gui::Button("Stop Motion",
                                             [this]()
                                             {
                                              LOG_WARNING("[ForceSensorCalibration] Motion was interrupted before it's planned duration (" << dt_ << " / " << duration_ << ")");
                                              interrupted_ = true;
                                             }
                                            )
                         );
}


bool CalibrationMotion::run(mc_control::fsm::Controller & ctl_)
{
  auto & ctl = static_cast<ForceSensorCalibration &>(ctl_);
  if(output() == "FAILURE")
  {
    return true;
  }

  // Update all joint positions
  for(auto & updateJoint : jointUpdates_)
  {
    updateJoint();
  }

  if(interrupted_ || dt_ > duration_)
  {
    output("OK");
    return true;
  }
  else
  {
    dt_ += ctl_.timeStep;
  }
  return false;
}

void CalibrationMotion::teardown(mc_control::fsm::Controller & ctl_)
{
  auto & ctl = static_cast<ForceSensorCalibration &>(ctl_);

  auto postureTask = ctl_.getPostureTask(ctl_.robot().name());
  postureTask->stiffness(savedStiffness_);
  ctl_.gui()->removeElement({}, "Progress");
  ctl_.gui()->removeElement({}, "Stop Motion");
}

EXPORT_SINGLE_STATE("CalibrationMotion", CalibrationMotion)
