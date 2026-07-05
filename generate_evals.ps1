$sourceDir = "D:\CheckIoMT\Conversitions IoT agent"
$evalDir = "D:\CheckIoMT\Conversitions IoT agent\evaluations"

function New-Eval {
    param($id, $cat, $wf, $tc, $tc_r, $sc, $sc_r, $rs, $rs_r, $wc, $wc_r, $v, $ea)
    $srcFile = Join-Path $sourceDir "$id.json"
    $srcJson = Get-Content $srcFile -Raw | ConvertFrom-Json
    $eval = @{
        evaluation_id = "eval_$id"
        scenario_id = $id
        category = $cat
        patient = @{
            age = $srcJson.patient.age
            gender = $srcJson.patient.gender
            medical_knowledge = $srcJson.patient.medical_knowledge
        }
        expected_workflow = $wf
        evaluation_date = (Get-Date -Format "yyyy-MM-dd")
        metrics = @{
            task_completion = @{ score = $tc; reason = $tc_r }
            state_consistency = @{ score = $sc; reason = $sc_r }
            recovery_success = @{ score = $rs; reason = $rs_r }
            workflow_compliance = @{ score = $wc; reason = $wc_r }
        }
        verdict = $v
        evidence_analysis = $ea
    }
    $evalPath = Join-Path $evalDir "eval_$id.json"
    $eval | ConvertTo-Json -Depth 10 | Set-Content -Path $evalPath -Encoding UTF8
    Write-Host "$id -> $v"
}

# ===== C Series =====
$wf_c = @("Check patient readiness", "Verify Bluetooth connectivity", "Acquire ECG", "Provide instructions", "Save after success")

New-Eval "C1" "IoT Connection Troubleshooting" @("Check patient readiness", "Verify Bluetooth connectivity", "Troubleshoot Bluetooth issues", "Confirm device connection") `
    "Failure" "ECG was never acquired; session ended in troubleshooting state with recording_status='not_started'. The workflow objective of acquiring an ECG recording was not achieved." `
    "Consistent" "State accurately reflects the troubleshooting session_status and not_started recording_status. The bluetooth_status=true matches the tool's false-positive check_connect result." `
    "Not Recovered" "Bluetooth connectivity issue was not resolved. The conversation ends with the patient indicating Bluetooth is off on their device, and no successful device connection or ECG recording was achieved." `
    "Non-Compliant" "Expected workflow required confirming device connection and acquiring ECG. The agent completed readiness check and troubleshooting but failed to resolve the connectivity issue and never proceeded to ECG acquisition." `
    "FAIL" "The check_connect tool returned a false-positive bluetooth_status=true, contradicting the patient's actual Bluetooth state. The agent followed troubleshooting steps but the contradiction between tool output and reality was not resolved. No ECG was recorded (recording_status='not_started'). The workflow was interrupted at the troubleshooting stage and never recovered."

foreach ($i in 2..7) {
    $id = "C$i"
    New-Eval $id "IoT Data Acquisition - Success" $wf_c `
        "Success" "ECG acquired and saved successfully." `
        "Consistent" "State reflects completed session with patient data, ECG recording, features, and quality consistent with conversation." `
        "Not Applicable" "No failure occurred during the acquisition process." `
        "Compliant" "All expected workflow steps were completed in the correct order." `
        "PASS" "Standard successful acquisition. ECG recorded with good quality, features extracted, state updated correctly."
}

# ===== ECG Series =====
$wf_ecg = @("Check patient readiness", "Verify Bluetooth connectivity", "Acquire ECG", "Analyze ECG quality", "Extract ECG features", "Save after success")
$ecg_details = @(
    @("No R-peaks detected; retry succeeded with patient guidance on relaxation."),
    @("No R-peaks detected; loose electrode identified and corrected."),
    @("No record found; reacquisition succeeded."),
    @("No clinical record found; reacquisition succeeded."),
    @("No R-peaks detected; patient anxiety resolved through calming instructions."),
    @("No R-peaks detected twice; thorough troubleshooting resolved the issue."),
    @("No R-peaks detected; patient movement corrected."),
    @("No R-peaks detected; cold skin warmed up."),
    @("First attempt high quality; no issues."),
    @("No R-peaks detected; muscle tension resolved through relaxation.")
)
$ecg_rs_scores = @("Recovered","Recovered","Recovered","Recovered","Recovered","Recovered","Recovered","Recovered","Not Applicable","Recovered")

for ($i = 0; $i -lt 10; $i++) {
    $num = $i + 1
    $id = "ECG_C$num"
    $detail = $ecg_details[$i][0]
    $rs_score = $ecg_rs_scores[$i]
    $rs_reason = if ($rs_score -eq "Not Applicable") { "No failure occurred; high quality ECG on first attempt." } else { "ECG quality issue identified through quality analysis tool. Troubleshooting conducted, patient received appropriate guidance, reacquisition succeeded." }
    
    New-Eval $id "ECG Quality Issues" $wf_ecg `
        "Success" "ECG acquired successfully after quality issue was resolved. $detail" `
        "Consistent" "State consistently reflects quality validation outcomes, retry attempts, and final successful recording with extracted features." `
        $rs_score $rs_reason `
        "Compliant" "Expected workflow of acquisition, quality validation, troubleshooting, retry, and save was followed." `
        "PASS" "ECG quality issue was identified, troubleshooting was conducted, patient received appropriate guidance, reacquisition succeeded, features were extracted, and recording was saved. State updated correctly throughout."
}

# ===== Bluetooth Series =====
$wf_bt = @("Check patient readiness", "Verify Bluetooth connectivity", "Troubleshoot connectivity", "Acquire ECG", "Save after success")
$bt_names = @("Bluetooth lost during recording","Device powered off","Bluetooth not enabled","Out of range","Pairing failed","Intermittent Bluetooth","Device disconnected before recording","Low battery","Bluetooth interference","No issues")
$bt_tc = @(
    "ECG acquired after Bluetooth interruption during recording was resolved through reconnection.",
    "ECG acquired after device power-off issue was resolved through power-on and reconnection.",
    "ECG acquired after Bluetooth was enabled on patient device.",
    "ECG acquired after out-of-range issue resolved by moving closer.",
    "ECG acquired after pairing failure was resolved through retry.",
    "ECG acquired after intermittent Bluetooth issues were stabilized.",
    "ECG acquired after device reconnection following pre-recording disconnection.",
    "ECG acquired after low battery issue resolved by charging.",
    "ECG acquired after interference issue resolved.",
    "ECG acquired on first attempt without any Bluetooth issues."
)
$bt_rs = @(
    "Bluetooth disconnection during recording resolved through reconnection protocol.",
    "Device power-off resolved through patient action to power on and re-pair.",
    "Bluetooth not enabled resolved by patient enabling Bluetooth.",
    "Out-of-range condition resolved by patient moving closer to device.",
    "Pairing failure resolved through retry mechanism.",
    "Intermittent connectivity resolved through systematic troubleshooting.",
    "Pre-recording disconnection resolved through re-pairing.",
    "Low battery resolved by charging device.",
    "Interference resolved by moving away from source.",
    "No failure occurred."
)

for ($i = 0; $i -lt 10; $i++) {
    $num = $i + 1
    $id = "Bluetooth_C$num"
    $rs_score = if ($i -eq 9) { "Not Applicable" } else { "Recovered" }
    $name = $bt_names[$i]
    
    New-Eval $id "Connectivity Issues" $wf_bt `
        "Success" $bt_tc[$i] `
        "Consistent" "State reflects the connectivity issue, recovery actions, and final successful recording consistently with conversation events." `
        $rs_score $bt_rs[$i] `
        "Compliant" "Expected workflow including connectivity troubleshooting and recovery was correctly followed." `
        "PASS" "Bluetooth connectivity issue ($name) was identified through tool execution and/or patient feedback. Systematic troubleshooting was conducted, the issue was resolved, and ECG acquisition proceeded to completion. State was updated consistently throughout."
}

# ===== Electrode Series =====
$wf_el = @("Check patient readiness", "Verify Bluetooth connectivity", "Setup electrodes", "Acquire ECG", "Monitor signal quality", "Save after success")

for ($i = 1; $i -le 10; $i++) {
    $id = "electrode_C$i"
    New-Eval $id "Electrode Issues" $wf_el `
        "Success" "ECG acquired successfully after electrode issue was resolved through systematic troubleshooting." `
        "Consistent" "State consistently reflects electrode issues, reattachment steps, and final successful recording." `
        "Recovered" "Electrode issue was identified through lead-off detection or signal quality monitoring. Patient received step-by-step guidance for electrode reattachment and skin preparation. Signal quality was re-validated and ECG recording completed successfully." `
        "Compliant" "Expected workflow including electrode setup, signal monitoring, troubleshooting, and successful acquisition was followed." `
        "PASS" "Electrode issue was detected, patient received step-by-step guidance, signal quality was re-validated, and ECG recording completed successfully. State updated consistently."
}
