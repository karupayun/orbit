// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "processlauncherwidget.h"

#include <QFileDialog>
#include <QLineEdit>

#include "../OrbitCore/Params.h"
#include "App.h"
#include "absl/flags/flag.h"
#include "ui_processlauncherwidget.h"

ABSL_DECLARE_FLAG(bool, enable_stale_features);

ProcessLauncherWidget::ProcessLauncherWidget(QWidget* parent)
    : QWidget(parent), ui(new Ui::ProcessLauncherWidget) {
  ui->setupUi(this);
  ui->ProcessComboBox->lineEdit()->setPlaceholderText("Process");
  ui->WorkingDirComboBox->lineEdit()->setPlaceholderText("Working Directory");
  ui->ArgumentsComboBox->lineEdit()->setPlaceholderText("Arguments");

  ui->gridLayout_2->setColumnStretch(0, 90);
  ui->checkBoxPause->setChecked(GParams.m_StartPaused);

  ui->ProcessComboBox->lineEdit()->setText("");

  if (!absl::GetFlag(FLAGS_enable_stale_features)) {
    ui->frame->setVisible(false);
  }
}

ProcessLauncherWidget::~ProcessLauncherWidget() { delete ui; }

void ProcessLauncherWidget::SetDataView(DataView* data_view) {
  ui->LiveProcessList->Initialize(data_view, SelectionType::kDefault,
                                  FontType::kDefault);

  if (GParams.m_ProcessFilter != "") {
    ui->LiveProcessList->SetFilter(GParams.m_ProcessFilter.c_str());
  }
}

void ProcessLauncherWidget::Refresh() { ui->LiveProcessList->Refresh(); }

void ProcessLauncherWidget::SetProcessParams() {
  if (GParams.m_ProcessPath != "") {
    ui->ProcessComboBox->lineEdit()->setText(GParams.m_ProcessPath.c_str());
  }
  if (GParams.m_WorkingDirectory != "") {
    ui->WorkingDirComboBox->lineEdit()->setText(
        GParams.m_WorkingDirectory.c_str());
  }
  if (GParams.m_Arguments != "") {
    ui->ArgumentsComboBox->lineEdit()->setText(GParams.m_Arguments.c_str());
  }
}

void ProcessLauncherWidget::UpdateProcessParams() {
  GParams.m_ProcessPath = ui->ProcessComboBox->lineEdit()->text().toStdString();
  GParams.m_Arguments = ui->ArgumentsComboBox->lineEdit()->text().toStdString();
  GParams.m_WorkingDirectory =
      ui->WorkingDirComboBox->lineEdit()->text().toStdString();
}

void ProcessLauncherWidget::on_BrowseButton_clicked() {
  QStringList list = QFileDialog::getOpenFileNames(
      this, "Select an executable file...", "", "*.exe");
  for (auto& file : list) {
    ui->ProcessComboBox->lineEdit()->setText(file);
    break;
  }
}

void ProcessLauncherWidget::on_LaunchButton_clicked() {
  QString process = ui->ProcessComboBox->lineEdit()->text();
  QString workingDir = ui->WorkingDirComboBox->lineEdit()->text();
  QString args = ui->ArgumentsComboBox->lineEdit()->text();
  GOrbitApp->OnLaunchProcess(process.toStdString(), workingDir.toStdString(),
                             args.toStdString());
}

void ProcessLauncherWidget::on_checkBoxPause_clicked(bool checked) {
  GParams.m_StartPaused = checked;
  GParams.Save();
}

void ProcessLauncherWidget::on_BrowseWorkingDirButton_clicked() {
  QString dir = QFileDialog::getExistingDirectory(
      this, tr("Open Directory"), "/home",
      QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  ui->WorkingDirComboBox->lineEdit()->setText(dir);
}
