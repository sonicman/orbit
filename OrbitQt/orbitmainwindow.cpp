// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "orbitmainwindow.h"

#include <QBoxLayout>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QProgressDialog>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QToolTip>
#include <QWidget>
#include <utility>

#include "App.h"
#include "CallTreeViewItemModel.h"
#include "OrbitClientModel/CaptureSerializer.h"
#include "OrbitVersion/OrbitVersion.h"
#include "Path.h"
#include "SamplingReport.h"
#include "StatusListenerImpl.h"
#include "TutorialContent.h"
#include "TutorialOverlay.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "orbitaboutdialog.h"
#include "orbitcodeeditor.h"
#include "orbitdisassemblydialog.h"
#include "orbitlivefunctions.h"
#include "orbitsamplingreport.h"
#include "services.pb.h"
#include "ui_orbitmainwindow.h"

ABSL_DECLARE_FLAG(bool, enable_stale_features);
ABSL_DECLARE_FLAG(bool, devmode);
ABSL_DECLARE_FLAG(bool, enable_tracepoint_feature);
ABSL_DECLARE_FLAG(bool, enable_tutorials_feature);

using orbit_grpc_protos::CrashOrbitServiceRequest_CrashType;
using orbit_grpc_protos::CrashOrbitServiceRequest_CrashType_CHECK_FALSE;
using orbit_grpc_protos::CrashOrbitServiceRequest_CrashType_NULL_POINTER_DEREFERENCE;
using orbit_grpc_protos::CrashOrbitServiceRequest_CrashType_STACK_OVERFLOW;

extern QMenu* GContextMenu;

OrbitMainWindow::OrbitMainWindow(QApplication* a_App,
                                 OrbitQt::ServiceDeployManager* service_deploy_manager,
                                 uint32_t font_size, const QString& target_label_message)
    : QMainWindow(nullptr), m_App(a_App), ui(new Ui::OrbitMainWindow) {
  DataViewFactory* data_view_factory = GOrbitApp.get();

  ui->setupUi(this);
  // Cannot save a capture as long as no capture has been loaded or captured.
  ui->actionSave_Capture->setDisabled(true);

  // ui->ProcessesList->SetDataView(data_view_factory->GetOrCreateDataView(DataViewType::kProcesses));

  QList<int> sizes;
  sizes.append(5000);
  sizes.append(5000);
  // ui->HomeVerticalSplitter->setSizes(sizes);
  // ui->HomeHorizontalSplitter->setSizes(sizes);
  ui->splitter_2->setSizes(sizes);

  status_listener_ = StatusListenerImpl::Create(statusBar());

  GOrbitApp->SetStatusListener(status_listener_.get());

  GOrbitApp->SetCaptureStartedCallback([this] {
    ui->actionToggle_Capture->setIcon(icon_stop_capture_);
    ui->actionClear_Capture->setDisabled(true);
    ui->actionOpen_Capture->setDisabled(true);
    ui->actionSave_Capture->setDisabled(true);
    ui->actionOpen_Preset->setDisabled(true);
    ui->actionSave_Preset_As->setDisabled(true);
    ui->symbolsBox->setDisabled(true);
    setWindowTitle({});
    hint_frame_->setVisible(false);
  });

  constexpr const char* kFinalizingCaptureMessage =
      "<div align=\"left\">"
      "Please wait while the capture is being finalized..."
      "<ul>"
      "<li>Waiting for the remaining capture data</li>"
      "<li>Processing callstacks</li>"
      "<li>Cleaning up dynamic instrumentation</li>"
      "</ul>"
      "</div>";
  auto finalizing_capture_dialog =
      new QProgressDialog(kFinalizingCaptureMessage, "OK", 0, 0, this, Qt::Tool);
  finalizing_capture_dialog->setWindowTitle("Finalizing capture");
  finalizing_capture_dialog->setModal(true);
  finalizing_capture_dialog->setWindowFlags(
      (finalizing_capture_dialog->windowFlags() | Qt::CustomizeWindowHint) &
      ~Qt::WindowCloseButtonHint & ~Qt::WindowSystemMenuHint);
  finalizing_capture_dialog->setFixedSize(finalizing_capture_dialog->size());
  finalizing_capture_dialog->close();

  GOrbitApp->SetCaptureStopRequestedCallback([this, finalizing_capture_dialog] {
    ui->actionToggle_Capture->setDisabled(true);
    finalizing_capture_dialog->show();
  });
  auto capture_finished_callback = [this, finalizing_capture_dialog] {
    finalizing_capture_dialog->close();
    ui->actionToggle_Capture->setDisabled(false);
    ui->actionToggle_Capture->setIcon(icon_start_capture_);
    ui->actionClear_Capture->setDisabled(false);
    ui->actionOpen_Capture->setDisabled(false);
    ui->actionSave_Capture->setDisabled(false);
    ui->actionOpen_Preset->setDisabled(false);
    ui->actionSave_Preset_As->setDisabled(false);
    ui->symbolsBox->setDisabled(false);
    LOG("Capture finished callback called");
  };
  GOrbitApp->SetCaptureStoppedCallback(capture_finished_callback);
  GOrbitApp->SetCaptureFailedCallback(capture_finished_callback);
  GOrbitApp->SetCaptureClearedCallback([this] { OnCaptureCleared(); });

  auto loading_capture_dialog =
      new QProgressDialog("Waiting for the capture to be loaded...", nullptr, 0, 0, this, Qt::Tool);
  loading_capture_dialog->setWindowTitle("Loading capture");
  loading_capture_dialog->setModal(true);
  loading_capture_dialog->setWindowFlags(
      (loading_capture_dialog->windowFlags() | Qt::CustomizeWindowHint) &
      ~Qt::WindowCloseButtonHint & ~Qt::WindowSystemMenuHint);
  loading_capture_dialog->setFixedSize(loading_capture_dialog->size());

  auto loading_capture_cancel_button = QPointer{new QPushButton{this}};
  loading_capture_cancel_button->setText("Cancel");
  QObject::connect(loading_capture_cancel_button, &QPushButton::clicked, this,
                   [loading_capture_dialog]() {
                     GOrbitApp->OnLoadCaptureCancelRequested();
                     loading_capture_dialog->close();
                   });
  loading_capture_dialog->setCancelButton(loading_capture_cancel_button);

  loading_capture_dialog->close();

  GOrbitApp->SetOpenCaptureCallback([this, loading_capture_dialog] {
    setWindowTitle({});
    loading_capture_dialog->show();
  });
  GOrbitApp->SetOpenCaptureFailedCallback([this, loading_capture_dialog] {
    setWindowTitle({});
    loading_capture_dialog->close();
  });
  GOrbitApp->SetOpenCaptureFinishedCallback(
      [loading_capture_dialog] { loading_capture_dialog->close(); });

  GOrbitApp->SetRefreshCallback([this](DataViewType type) {
    if (type == DataViewType::kAll || type == DataViewType::kLiveFunctions) {
      this->ui->liveFunctions->OnDataChanged();
    }
    this->OnRefreshDataViewPanels(type);
  });

  GOrbitApp->SetSamplingReportCallback(
      [this](DataView* callstack_data_view, std::shared_ptr<SamplingReport> report) {
        this->OnNewSamplingReport(callstack_data_view, std::move(report));
      });

  ui->RightTabWidget->setTabEnabled(ui->RightTabWidget->indexOf(ui->selectionSamplingTab), false);
  GOrbitApp->SetSelectionReportCallback(
      [this](DataView* callstack_data_view, std::shared_ptr<SamplingReport> report) {
        this->OnNewSelectionReport(callstack_data_view, std::move(report));
      });

  GOrbitApp->SetTopDownViewCallback([this](std::unique_ptr<CallTreeView> top_down_view) {
    this->OnNewTopDownView(std::move(top_down_view));
  });

  ui->RightTabWidget->setTabEnabled(ui->RightTabWidget->indexOf(ui->selectionTopDownTab), false);
  GOrbitApp->SetSelectionTopDownViewCallback(
      [this](std::unique_ptr<CallTreeView> selection_top_down_view) {
        this->OnNewSelectionTopDownView(std::move(selection_top_down_view));
      });

  GOrbitApp->SetBottomUpViewCallback([this](std::unique_ptr<CallTreeView> bottom_up_view) {
    this->OnNewBottomUpView(std::move(bottom_up_view));
  });

  ui->RightTabWidget->setTabEnabled(ui->RightTabWidget->indexOf(ui->selectionBottomUpTab), false);
  GOrbitApp->SetSelectionBottomUpViewCallback(
      [this](std::unique_ptr<CallTreeView> selection_bottom_up_view) {
        this->OnNewSelectionBottomUpView(std::move(selection_bottom_up_view));
      });

  GOrbitApp->SetSelectLiveTabCallback(
      [this] { ui->RightTabWidget->setCurrentWidget(ui->liveTab); });
  GOrbitApp->SetDisassemblyCallback([this](std::string disassembly, DisassemblyReport report) {
    OpenDisassembly(std::move(disassembly), std::move(report));
  });
  GOrbitApp->SetErrorMessageCallback([this](const std::string& title, const std::string& text) {
    QMessageBox::critical(this, QString::fromStdString(title), QString::fromStdString(text));
  });
  GOrbitApp->SetWarningMessageCallback([this](const std::string& title, const std::string& text) {
    QMessageBox::warning(this, QString::fromStdString(title), QString::fromStdString(text));
  });
  GOrbitApp->SetInfoMessageCallback([this](const std::string& title, const std::string& text) {
    QMessageBox::information(this, QString::fromStdString(title), QString::fromStdString(text));
  });
  GOrbitApp->SetTooltipCallback([this](const std::string& tooltip) {
    QToolTip::showText(QCursor::pos(), QString::fromStdString(tooltip), this);
  });
  GOrbitApp->SetSaveFileCallback(
      [this](const std::string& extension) { return this->OnGetSaveFileName(extension); });
  GOrbitApp->SetClipboardCallback([this](const std::string& text) { this->OnSetClipboard(text); });

  GOrbitApp->SetSecureCopyCallback([service_deploy_manager](std::string_view source,
                                                            std::string_view destination) {
    CHECK(service_deploy_manager != nullptr);
    return service_deploy_manager->CopyFileToLocal(std::string{source}, std::string{destination});
  });

  GOrbitApp->SetDisableSymbolsCallback([this] {
    // ui->symbolsBox->setDisabled(true);
    LOG("symbols box enabled: %d", ui->symbolsBox->isEnabled());
  });

  ui->CaptureGLWidget->Initialize(GlCanvas::CanvasType::kCaptureWindow, this, font_size);

  hint_frame_ = new QFrame();
  hint_frame_->setStyleSheet("background: transparent");
  auto* hint_layout = new QVBoxLayout();
  hint_layout->setSpacing(0);
  hint_layout->setMargin(0);
  hint_frame_->setLayout(hint_layout);
  auto* hint_arrow = new QLabel();
  hint_arrow->setPixmap(QPixmap(":/images/grey_arrow_up.png").scaledToHeight(12));
  hint_layout->addWidget(hint_arrow);
  auto* hint_message = new QLabel("Start a capture here");
  hint_message->setAlignment(Qt::AlignCenter);
  hint_layout->addWidget(hint_message);
  hint_message->setStyleSheet(
      "background-color: rgb(117, 117, 117);"
      "border-top-left-radius: 1px;"
      "border-top-right-radius: 4px;"
      "border-bottom-right-radius: 4px;"
      "border-bottom-left-radius: 4px;");
  hint_layout->setStretchFactor(hint_message, 1);

  // TODO maybe there is a better way
  hint_frame_->setParent(ui->captureFrame);

  hint_frame_->move(17, 42);
  hint_frame_->resize(140, 45);

  if (absl::GetFlag(FLAGS_devmode)) {
    ui->debugOpenGLWidget->Initialize(GlCanvas::CanvasType::kDebug, this, font_size);
    GOrbitApp->SetDebugCanvas(ui->debugOpenGLWidget->GetCanvas());
  } else {
    ui->RightTabWidget->removeTab(ui->RightTabWidget->indexOf(ui->debugTab));
  }

  ui->ModulesList->Initialize(data_view_factory->GetOrCreateDataView(DataViewType::kModules),
                              SelectionType::kExtended, FontType::kDefault);
  ui->FunctionsList->Initialize(data_view_factory->GetOrCreateDataView(DataViewType::kFunctions),
                                SelectionType::kExtended, FontType::kDefault);
  ui->CallStackView->Initialize(data_view_factory->GetOrCreateDataView(DataViewType::kCallstack),
                                SelectionType::kExtended, FontType::kDefault);
  ui->PresetsList->Initialize(data_view_factory->GetOrCreateDataView(DataViewType::kPresets),
                              SelectionType::kDefault, FontType::kDefault,
                              /*is_main_instance=*/true, /*uniform_row_height=*/false,
                              /*text_alignment=*/Qt::AlignTop | Qt::AlignLeft);
  ui->TracepointsList->Initialize(
      data_view_factory->GetOrCreateDataView(DataViewType::kTracepoints), SelectionType::kExtended,
      FontType::kDefault);

  SetupCodeView();

  if (!absl::GetFlag(FLAGS_enable_stale_features)) {
    ui->RightTabWidget->removeTab(ui->RightTabWidget->indexOf(ui->CallStackTab));
    ui->RightTabWidget->removeTab(ui->RightTabWidget->indexOf(ui->CodeTab));
  }

  if (!absl::GetFlag(FLAGS_enable_tracepoint_feature)) {
    ui->RightTabWidget->removeTab(ui->RightTabWidget->indexOf(ui->tracepointsTab));
  }

  if (!absl::GetFlag(FLAGS_devmode)) {
    ui->menuDebug->menuAction()->setVisible(false);
  }

  if (absl::GetFlag(FLAGS_enable_tutorials_feature)) {
    InitTutorials(this);
  }

  auto* target_widget = new QWidget();
  target_widget->setStyleSheet("background-color: rgb(68,68,68)");
  auto* target_label = new QLabel(target_label_message);
  target_label->setContentsMargins(6, 0, 0, 0);
  if (GOrbitApp->GetSelectedProcess() != nullptr) {
    target_label->setStyleSheet("color: rgb(41,218,130)");
  }
  auto* disconnect_target_button = new QPushButton("End Session");
  auto* target_layout = new QHBoxLayout();
  target_layout->addWidget(target_label);
  target_layout->addWidget(disconnect_target_button);
  target_layout->setMargin(0);
  target_widget->setLayout(target_layout);

  ui->menuBar->setCornerWidget(target_widget, Qt::TopRightCorner);

  QObject::connect(disconnect_target_button, &QPushButton::clicked, this, [this]() {
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, QApplication::applicationName(),
        "This discards any unsaved progress. Are you sure you want to continue?");

    if (reply == QMessageBox::Yes) {
      QApplication::exit(1);
    }
  });

  SetupCaptureToolbar();

  StartMainTimer();

  ui->liveFunctions->Initialize(SelectionType::kExtended, FontType::kDefault);

  connect(ui->liveFunctions->GetFilterLineEdit(), &QLineEdit::textChanged, this,
          [this](const QString& text) { OnLiveTabFunctionsFilterTextChanged(text); });

  ui->topDownWidget->Initialize(GOrbitApp.get());
  ui->selectionTopDownWidget->Initialize(GOrbitApp.get());
  ui->bottomUpWidget->Initialize(GOrbitApp.get());
  ui->selectionBottomUpWidget->Initialize(GOrbitApp.get());

  setWindowTitle({});
  std::string iconFileName = Path::JoinPath({Path::GetExecutableDir(), "orbit.ico"});
  this->setWindowIcon(QIcon(iconFileName.c_str()));

  GOrbitApp->PostInit();
}

static QWidget* CreateSpacer(QWidget* parent) {
  auto* spacer = new QLabel(parent);
  spacer->setText("    ");
  return spacer;
}

void OrbitMainWindow::SetupCaptureToolbar() {
  // Sizes.
  QToolBar* toolbar = ui->capture_toolbar;

  // Create missing icons
  icon_start_capture_ = QIcon(":/actions/play_arrow");
  icon_stop_capture_ = QIcon(":/actions/stop");

  // Attach the filter panel to the toolbar
  toolbar->addWidget(CreateSpacer(toolbar));
  toolbar->addWidget(ui->filterPanel);

  // Timer
  toolbar->addWidget(CreateSpacer(toolbar));
  QFontMetrics fm(ui->timerLabel->font());
  int pixel_width = fm.width("w");
  ui->timerLabel->setMinimumWidth(5 * pixel_width);
}

void OrbitMainWindow::SetupCodeView() {
  ui->CodeTextEdit->SetEditorType(OrbitCodeEditor::CODE_VIEW);
  ui->FileMappingTextEdit->SetEditorType(OrbitCodeEditor::FILE_MAPPING);
  ui->FileMappingTextEdit->SetSaveButton(ui->SaveFileMapping);
  ui->CodeTextEdit->SetFindLineEdit(ui->lineEdit);
  ui->FileMappingWidget->hide();
  OrbitCodeEditor::setFileMappingWidget(ui->FileMappingWidget);
}

OrbitMainWindow::~OrbitMainWindow() {
  DeinitTutorials();
  delete ui;
}

void OrbitMainWindow::OnRefreshDataViewPanels(DataViewType a_Type) {
  if (a_Type == DataViewType::kAll) {
    for (int i = 0; i < static_cast<int>(DataViewType::kAll); ++i) {
      UpdatePanel(static_cast<DataViewType>(i));
    }
  } else {
    UpdatePanel(a_Type);
  }
}

void OrbitMainWindow::UpdatePanel(DataViewType a_Type) {
  switch (a_Type) {
    case DataViewType::kCallstack:
      ui->CallStackView->Refresh();
      break;
    case DataViewType::kFunctions:
      ui->FunctionsList->Refresh();
      break;
    case DataViewType::kLiveFunctions:
      ui->liveFunctions->Refresh();
      break;
    case DataViewType::kModules:
      ui->ModulesList->Refresh();
      break;
    case DataViewType::kProcesses:
      ERROR("TODO: remove");
      // ui->ProcessesList->Refresh();
      break;
    case DataViewType::kPresets:
      ui->PresetsList->Refresh();
      break;
    case DataViewType::kSampling:
      ui->samplingReport->RefreshCallstackView();
      ui->samplingReport->RefreshTabs();
      ui->selectionReport->RefreshCallstackView();
      ui->selectionReport->RefreshTabs();
      break;
    default:
      break;
  }
}

void OrbitMainWindow::OnNewSamplingReport(DataView* callstack_data_view,
                                          std::shared_ptr<SamplingReport> sampling_report) {
  ui->samplingGridLayout->removeWidget(ui->samplingReport);
  delete ui->samplingReport;

  ui->samplingReport = new OrbitSamplingReport(ui->samplingTab);
  ui->samplingReport->Initialize(callstack_data_view, sampling_report);
  ui->samplingGridLayout->addWidget(ui->samplingReport, 0, 0, 1, 1);

  // Switch to sampling tab if sampling report is not empty and if not already in live tab.
  bool has_samples = sampling_report->HasSamples();
  if (has_samples && (ui->RightTabWidget->currentWidget() != ui->liveTab)) {
    ui->RightTabWidget->setCurrentWidget(ui->samplingTab);
  }
}

void OrbitMainWindow::OnNewSelectionReport(DataView* callstack_data_view,
                                           std::shared_ptr<SamplingReport> sampling_report) {
  if (sampling_report->HasSamples()) {
    ui->RightTabWidget->setTabEnabled(ui->RightTabWidget->indexOf(ui->selectionSamplingTab), true);
    // This condition and the corresponding ones in OnNewSelectionTopDownView,
    // OnNewSelectionBottomUpView need to be complementary, such that one doesn't cause switching
    // away from or to a tab that the other method would switch from when such a tab is selected.
    // Otherwise, which tab ends up being selected would depend on the order in which these two
    // methods are called.
    if (ui->RightTabWidget->currentWidget() != ui->topDownTab &&
        ui->RightTabWidget->currentWidget() != ui->selectionTopDownTab &&
        ui->RightTabWidget->currentWidget() != ui->bottomUpTab &&
        ui->RightTabWidget->currentWidget() != ui->selectionBottomUpTab) {
      ui->RightTabWidget->setCurrentWidget(ui->selectionSamplingTab);
    }
  } else {
    // If the selection is empty, if this tab is currently selected switch to the corresponding tab
    // for the entire capture...
    if (ui->RightTabWidget->currentWidget() == ui->selectionSamplingTab) {
      ui->RightTabWidget->setCurrentWidget(ui->samplingTab);
    }
    // ...and then disable this tab.
    ui->RightTabWidget->setTabEnabled(ui->RightTabWidget->indexOf(ui->selectionSamplingTab), false);
  }

  ui->selectionGridLayout->removeWidget(ui->selectionReport);
  delete ui->selectionReport;

  ui->selectionReport = new OrbitSamplingReport(ui->selectionSamplingTab);
  ui->selectionReport->Initialize(callstack_data_view, std::move(sampling_report));
  ui->selectionGridLayout->addWidget(ui->selectionReport, 0, 0, 1, 1);
}

void OrbitMainWindow::OnNewTopDownView(std::unique_ptr<CallTreeView> top_down_view) {
  ui->topDownWidget->SetTopDownView(std::move(top_down_view));
}

void OrbitMainWindow::OnNewSelectionTopDownView(
    std::unique_ptr<CallTreeView> selection_top_down_view) {
  if (selection_top_down_view->child_count() > 0) {
    ui->RightTabWidget->setTabEnabled(ui->RightTabWidget->indexOf(ui->selectionTopDownTab), true);
    // This condition and the corresponding ones in OnNewSelectionReport, OnNewSelectionBottomUpView
    // need to be complementary, such that one doesn't cause switching away from or to a tab that
    // the other method would switch from when such a tab is selected. Otherwise, which tab ends up
    // being selected would depend on the order in which these two methods are called.
    if (ui->RightTabWidget->currentWidget() == ui->topDownTab) {
      ui->RightTabWidget->setCurrentWidget(ui->selectionTopDownTab);
    }
  } else {
    // If the selection is empty, if this tab is currently selected switch to the corresponding tab
    // for the entire capture...
    if (ui->RightTabWidget->currentWidget() == ui->selectionTopDownTab) {
      ui->RightTabWidget->setCurrentWidget(ui->topDownTab);
    }
    // ...and then disable this tab.
    ui->RightTabWidget->setTabEnabled(ui->RightTabWidget->indexOf(ui->selectionTopDownTab), false);
  }
  ui->selectionTopDownWidget->SetTopDownView(std::move(selection_top_down_view));
}

void OrbitMainWindow::OnNewBottomUpView(std::unique_ptr<CallTreeView> bottom_up_view) {
  ui->bottomUpWidget->SetBottomUpView(std::move(bottom_up_view));
}

void OrbitMainWindow::OnNewSelectionBottomUpView(
    std::unique_ptr<CallTreeView> selection_bottom_up_view) {
  if (selection_bottom_up_view->child_count() > 0) {
    ui->RightTabWidget->setTabEnabled(ui->RightTabWidget->indexOf(ui->selectionBottomUpTab), true);
    // This condition and the corresponding ones in OnNewSelectionReport, OnNewSelectionTopDownView
    // need to be complementary, such that one doesn't cause switching away from or to a tab that
    // the other method would switch from when such a tab is selected. Otherwise, which tab ends up
    // being selected would depend on the order in which these two methods are called.
    if (ui->RightTabWidget->currentWidget() == ui->bottomUpTab) {
      ui->RightTabWidget->setCurrentWidget(ui->selectionBottomUpTab);
    }
  } else {
    // If the selection is empty, if this tab is currently selected switch to the corresponding tab
    // for the entire capture...
    if (ui->RightTabWidget->currentWidget() == ui->selectionBottomUpTab) {
      ui->RightTabWidget->setCurrentWidget(ui->bottomUpTab);
    }
    // ...and then disable this tab.
    ui->RightTabWidget->setTabEnabled(ui->RightTabWidget->indexOf(ui->selectionBottomUpTab), false);
  }
  ui->selectionBottomUpWidget->SetBottomUpView(std::move(selection_bottom_up_view));
}

std::string OrbitMainWindow::OnGetSaveFileName(const std::string& extension) {
  std::string filename =
      QFileDialog::getSaveFileName(this, "Specify a file to save...", nullptr, extension.c_str())
          .toStdString();
  if (!filename.empty() && !absl::EndsWith(filename, extension)) {
    filename += extension;
  }
  return filename;
}

void OrbitMainWindow::OnSetClipboard(const std::string& text) {
  QApplication::clipboard()->setText(QString::fromStdString(text));
}

void OrbitMainWindow::on_actionReport_Missing_Feature_triggered() {
  if (!QDesktopServices::openUrl(
          QUrl("https://community.stadia.dev/s/feature-requests", QUrl::StrictMode))) {
    QMessageBox::critical(this, "Error opening URL",
                          "Could not open community.stadia.dev/s/feature-request");
  }
}

void OrbitMainWindow::on_actionReport_Bug_triggered() {
  if (!QDesktopServices::openUrl(
          QUrl("https://community.stadia.dev/s/contactsupport", QUrl::StrictMode))) {
    QMessageBox::critical(this, "Error opening URL",
                          "Could not open community.stadia.dev/s/contactsupport");
  }
}

void OrbitMainWindow::on_actionAbout_triggered() {
  OrbitQt::OrbitAboutDialog dialog{this};
  dialog.setWindowTitle("About");
  dialog.SetVersionString(QCoreApplication::applicationVersion());
  dialog.SetBuildInformation(QString::fromStdString(OrbitCore::GetBuildReport()));

  QFile licenseFile{QDir{QCoreApplication::applicationDirPath()}.filePath("NOTICE")};
  if (licenseFile.open(QIODevice::ReadOnly)) {
    dialog.SetLicenseText(licenseFile.readAll());
  }
  dialog.exec();
}

void OrbitMainWindow::StartMainTimer() {
  m_MainTimer = new QTimer(this);
  connect(m_MainTimer, SIGNAL(timeout()), this, SLOT(OnTimer()));

  // Update period set to 16ms (~60FPS)
  int msec = 16;
  m_MainTimer->start(msec);
}

void OrbitMainWindow::OnTimer() {
  GOrbitApp->MainTick();

  for (OrbitGLWidget* glWidget : m_GlWidgets) {
    glWidget->update();
  }

  ui->timerLabel->setText(QString::fromStdString(GOrbitApp->GetCaptureTime()));
}

void OrbitMainWindow::OnFilterFunctionsTextChanged(const QString& text) {
  // The toolbar and live tab filters are mirrored.
  ui->liveFunctions->SetFilter(text);
}

void OrbitMainWindow::OnLiveTabFunctionsFilterTextChanged(const QString& text) {
  // Set main toolbar functions filter without triggering signals.
  ui->filterFunctions->blockSignals(true);
  ui->filterFunctions->setText(text);
  ui->filterFunctions->blockSignals(false);
}

void OrbitMainWindow::OnFilterTracksTextChanged(const QString& text) {
  GOrbitApp->FilterTracks(text.toStdString());
}

void OrbitMainWindow::on_actionOpen_Preset_triggered() {
  QStringList list = QFileDialog::getOpenFileNames(this, "Select a file to open...",
                                                   Path::CreateOrGetPresetDir().c_str(), "*.opr");
  for (const auto& file : list) {
    ErrorMessageOr<void> result = GOrbitApp->OnLoadPreset(file.toStdString());
    if (result.has_error()) {
      QMessageBox::critical(this, "Error loading session",
                            absl::StrFormat("Could not load session from \"%s\":\n%s.",
                                            file.toStdString(), result.error().message())
                                .c_str());
    }
    break;
  }
}

void OrbitMainWindow::on_actionEnd_Session_triggered() { QApplication::exit(1); }

void OrbitMainWindow::on_actionQuit_triggered() {
  close();
  QApplication::quit();
}

QPixmap QtGrab(OrbitMainWindow* a_Window) {
  QPixmap pixMap = a_Window->grab();
  if (GContextMenu) {
    QPixmap menuPixMap = GContextMenu->grab();
    pixMap.copy();
  }
  return pixMap;
}

void OrbitMainWindow::on_actionSave_Preset_As_triggered() {
  QString file = QFileDialog::getSaveFileName(this, "Specify a file to save...",
                                              Path::CreateOrGetPresetDir().c_str(), "*.opr");
  if (file.isEmpty()) {
    return;
  }

  ErrorMessageOr<void> result = GOrbitApp->OnSavePreset(file.toStdString());
  if (result.has_error()) {
    QMessageBox::critical(this, "Error saving session",
                          absl::StrFormat("Could not save session in \"%s\":\n%s.",
                                          file.toStdString(), result.error().message())
                              .c_str());
  }
}

void OrbitMainWindow::on_actionToggle_Capture_triggered() { GOrbitApp->ToggleCapture(); }

void OrbitMainWindow::on_actionClear_Capture_triggered() { GOrbitApp->ClearCapture(); }

void OrbitMainWindow::on_actionHelp_triggered() { GOrbitApp->ToggleDrawHelp(); }

void OrbitMainWindow::ShowCaptureOnSaveWarningIfNeeded() {
  QSettings settings("The Orbit Authors", "Orbit Profiler");
  const QString skip_capture_warning("SkipCaptureVersionWarning");
  if (!settings.value(skip_capture_warning, false).toBool()) {
    QMessageBox message_box;
    message_box.setText(
        "Note: Captures saved with this version of Orbit might be incompatible "
        "with future versions. Please check release notes for more "
        "information");
    message_box.addButton(QMessageBox::Ok);
    QCheckBox check_box("Don't show this message again.");
    message_box.setCheckBox(&check_box);

    QObject::connect(&check_box, &QCheckBox::stateChanged,
                     [&settings, &skip_capture_warning](int state) {
                       settings.setValue(skip_capture_warning, static_cast<bool>(state));
                     });

    message_box.exec();
  }
}

void OrbitMainWindow::on_actionSave_Capture_triggered() {
  ShowCaptureOnSaveWarningIfNeeded();

  const CaptureData& capture_data = GOrbitApp->GetCaptureData();
  QString file = QFileDialog::getSaveFileName(
      this, "Save capture...",
      Path::JoinPath(
          {Path::CreateOrGetCaptureDir(), capture_serializer::GetCaptureFileName(capture_data)})
          .c_str(),
      "*.orbit");
  if (file.isEmpty()) {
    return;
  }

  ErrorMessageOr<void> result = GOrbitApp->OnSaveCapture(file.toStdString());
  if (result.has_error()) {
    QMessageBox::critical(this, "Error saving capture",
                          absl::StrFormat("Could not save capture in \"%s\":\n%s.",
                                          file.toStdString(), result.error().message())
                              .c_str());
  }
}

void OrbitMainWindow::on_actionOpen_Capture_triggered() {
  QString file = QFileDialog::getOpenFileName(
      this, "Open capture...", QString::fromStdString(Path::CreateOrGetCaptureDir()), "*.orbit");
  if (file.isEmpty()) {
    return;
  }

  OpenCapture(file.toStdString());
}

void OrbitMainWindow::OpenCapture(const std::string& filepath) {
  GOrbitApp->OnLoadCapture(filepath);
  setWindowTitle(QString::fromStdString(filepath));
  // ui->MainTabWidget->setCurrentWidget(ui->CaptureTab);
}

void OrbitMainWindow::OpenDisassembly(std::string a_String, DisassemblyReport report) {
  auto* dialog = new OrbitDisassemblyDialog(this);
  dialog->SetText(std::move(a_String));
  dialog->SetDisassemblyReport(std::move(report));
  dialog->setWindowTitle("Orbit Disassembly");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowFlags(dialog->windowFlags() | Qt::WindowMinimizeButtonHint |
                         Qt::WindowMaximizeButtonHint);
  dialog->show();
}

void OrbitMainWindow::on_actionCheckFalse_triggered() { CHECK(false); }

void OrbitMainWindow::on_actionNullPointerDereference_triggered() {
  int* null_pointer = nullptr;
  *null_pointer = 0;
}

void InfiniteRecursion(int num) {
  if (num != 1) {
    InfiniteRecursion(num);
  }

  LOG("num=%d", num);
}

void OrbitMainWindow::on_actionStackOverflow_triggered() { InfiniteRecursion(0); }

void OrbitMainWindow::on_actionServiceCheckFalse_triggered() {
  GOrbitApp->CrashOrbitService(CrashOrbitServiceRequest_CrashType_CHECK_FALSE);
}

void OrbitMainWindow::on_actionServiceNullPointerDereference_triggered() {
  GOrbitApp->CrashOrbitService(CrashOrbitServiceRequest_CrashType_NULL_POINTER_DEREFERENCE);
}

void OrbitMainWindow::on_actionServiceStackOverflow_triggered() {
  GOrbitApp->CrashOrbitService(CrashOrbitServiceRequest_CrashType_STACK_OVERFLOW);
}

void OrbitMainWindow::OnCaptureCleared() {
  ui->liveFunctions->Reset();
  ui->actionSave_Capture->setDisabled(true);
  hint_frame_->setVisible(true);
}

void OrbitMainWindow::closeEvent(QCloseEvent* event) {
  if (GOrbitApp->IsCapturing()) {
    event->ignore();

    if (QMessageBox::question(this, "Capture in progress",
                              "A capture is currently in progress. Do you want to abort the "
                              "capture and exit Orbit?") == QMessageBox::Yes) {
      // We need for the capture to clean up - close as soon as this is done
      GOrbitApp->SetCaptureFailedCallback([&] { close(); });
      GOrbitApp->AbortCapture();
    }
  } else {
    QMainWindow::closeEvent(event);
  }
}
