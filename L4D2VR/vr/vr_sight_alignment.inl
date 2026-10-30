// Crosshair-to-sights alignment. The render pass publishes the drawn bore
// direction (weapon_bone +Z) in the viewmodel's own frame; once the weapon has
// settled, the main thread solves the extra viewmodel pitch/yaw that makes the
// bore parallel to the aim ray and keeps it per model.

void VR::PublishSightBore(const std::string& modelLower, const Vector& boreLocal)
{
    std::lock_guard<std::mutex> lock(m_SightBoreMutex);
    m_SightBoreModel = modelLower;
    m_SightBoreLocal = boreLocal;
    m_SightBoreAtMs = GetTickCount64();
    ++m_SightBoreSerial;
}

void VR::PublishSightPoints(const std::string& modelLower, const Vector& rearWorld, const Vector& frontWorld)
{
    const Vector handPos = GetRightControllerAbsPos();
    Vector forward{}, right{}, up{};
    QAngle::AngleVectors(GetRightControllerAbsAngle(), &forward, &right, &up);
    const Vector r = rearWorld - handPos, f = frontWorld - handPos;
    std::lock_guard<std::mutex> lock(m_SightBoreMutex);
    m_SightRearInHand = Vector(DotProduct(r, forward), DotProduct(r, right), DotProduct(r, up));
    m_SightFrontInHand = Vector(DotProduct(f, forward), DotProduct(f, right), DotProduct(f, up));
    m_SightPointsModel = modelLower;
    m_SightPointsAtMs = GetTickCount64();
}

bool VR::GetIronSightRay(Vector& origin, Vector& direction, Vector* frontOut) const
{
    if (!m_IronSightAimEnabled || !m_SightAutoAlignEnabled || m_MouseModeEnabled || m_IsThirdPersonCamera)
        return false;
    Vector rear{}, front{};
    {
        std::lock_guard<std::mutex> lock(m_SightBoreMutex);
        // Only while the drawn gun is the one being aimed (fresh, same model).
        if (m_SightPointsModel.empty() || m_SightPointsModel != m_SightBoreModel ||
            GetTickCount64() - m_SightPointsAtMs > 250u)
            return false;
        rear = m_SightRearInHand;
        front = m_SightFrontInHand;
    }
    const Vector& p = m_RightControllerPosAbs;
    const Vector& f = m_RightControllerForward;
    const Vector& r = m_RightControllerRight;
    const Vector& u = m_RightControllerUp;
    const Vector rearWorld = p + f * rear.x + r * rear.y + u * rear.z;
    const Vector frontWorld = p + f * front.x + r * front.y + u * front.z;
    Vector dir = frontWorld - rearWorld;
    if (VectorNormalize(dir) < 0.5f || !l4d2vr_calibration::Finite(rearWorld) || !l4d2vr_calibration::Finite(dir))
        return false;
    origin = rearWorld;
    direction = dir;
    if (frontOut) *frontOut = frontWorld;
    return true;
}

void VR::UpdateSightAlignment(const QAngle& baseOffset)
{
    m_SightActiveCorrection = QAngle(0.0f, 0.0f, 0.0f);
    if (!m_SightAutoAlignEnabled)
        return;

    std::string model;
    Vector bore{};
    uint64_t atMs = 0;
    uint32_t serial = 0;
    {
        std::lock_guard<std::mutex> lock(m_SightBoreMutex);
        model = m_SightBoreModel;
        bore = m_SightBoreLocal;
        atMs = m_SightBoreAtMs;
        serial = m_SightBoreSerial;
    }
    const uint64_t nowMs = GetTickCount64();
    if (model.empty() || nowMs - atMs > 250u)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (model != m_SightCalibrationModel)
    {
        m_SightCalibrationModel = model;
        m_SightCalibrationModelSince = now;
        m_SightBoreAverage.Reset();
    }

    auto found = m_SightCorrections.find(model);
    const bool calibrated = found != m_SightCorrections.end() &&
        std::fabs(found->second.baseOffset.x - baseOffset.x) < 0.05f &&
        std::fabs(found->second.baseOffset.y - baseOffset.y) < 0.05f &&
        std::fabs(found->second.baseOffset.z - baseOffset.z) < 0.05f;
    if (found != m_SightCorrections.end())
        m_SightActiveCorrection = found->second.correction;
    if (calibrated)
        return;

    // Gather samples only while nothing animates the gun away from its rest pose.
    const float age = std::chrono::duration<float>(now - m_SightCalibrationModelSince).count();
    const bool settled = age > 1.6f && !m_PrimaryAttackDown && !IsMagazineInteractionManualActive() &&
        m_HasAimLine && model.find("dual") == std::string::npos;
    if (!settled || serial == m_SightLastSerial)
        return;
    m_SightLastSerial = serial;
    m_SightBoreAverage.Add(bore);
    if (m_SightBoreAverage.count < 45)
        return;

    // The aim line now follows the sights themselves, so align the sights to
    // where the hand points (the controller's aim direction).
    Vector aim = m_RightControllerForward;
    const float length = VectorLength(aim);
    if (!std::isfinite(length) || length < 1.0f)
    {
        m_SightBoreAverage.Reset();
        return;
    }
    aim *= 1.0f / length;
    const Vector aimLocal(DotProduct(aim, m_RightControllerForward), -DotProduct(aim, m_RightControllerRight),
        DotProduct(aim, m_RightControllerUp));
    QAngle correction{};
    if (l4d2vr_sight_correction::Solve(baseOffset, m_SightBoreAverage.Mean(), aimLocal, correction))
    {
        m_SightCorrections[model] = SightCorrectionEntry{ correction, baseOffset };
        m_SightActiveCorrection = correction;
        Game::logMsg("[VR][SightAlign] model=%s correction pitch=%.2f yaw=%.2f (base %.2f %.2f %.2f)",
            model.c_str(), correction.x, correction.y, baseOffset.x, baseOffset.y, baseOffset.z);
    }
    else
    {
        Game::logMsg("[VR][SightAlign] model=%s solve rejected; keeping table offset", model.c_str());
        m_SightCorrections[model] = SightCorrectionEntry{ QAngle(0.0f, 0.0f, 0.0f), baseOffset };
    }
    m_SightBoreAverage.Reset();
}
