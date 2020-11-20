// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "orbitdataviewpanel.h"

#include <string>

#include "ui_orbitdataviewpanel.h"

OrbitDataViewPanel::OrbitDataViewPanel(QWidget* parent)
    : QWidget(parent), ui(new Ui::OrbitDataViewPanel) {
  ui->setupUi(this);
  ui->label->hide();
}

OrbitDataViewPanel::~OrbitDataViewPanel() { delete ui; }

void OrbitDataViewPanel::Initialize(DataView* data_view, SelectionType selection_type,
                                    FontType font_type, bool is_main_instance,
                                    bool uniform_row_height,
                                    QFlags<Qt::AlignmentFlag> text_alignment) {
  ui->treeView->Initialize(data_view, selection_type, font_type, uniform_row_height,
                           text_alignment);

  if (is_main_instance) {
    ui->treeView->GetModel()->GetDataView()->SetAsMainInstance();
  }

  std::string label = ui->treeView->GetLabel();
  if (!label.empty()) {
    this->ui->label->setText(QString::fromStdString(label));
    this->ui->label->show();
  }

  if (ui->treeView->HasRefreshButton()) {
    ui->refreshButton->show();
    ui->horizontalLayout->setContentsMargins(0, 0, 6, 0);
  } else {
    ui->refreshButton->hide();
  }

  data_view->SetUiFilterCallback([this](const std::string& filter) { SetFilter(filter.c_str()); });
}

OrbitTreeView* OrbitDataViewPanel::GetTreeView() { return ui->treeView; }

QLineEdit* OrbitDataViewPanel::GetFilterLineEdit() { return ui->FilterLineEdit; }

void OrbitDataViewPanel::Link(OrbitDataViewPanel* a_Panel) {
  ui->treeView->Link(a_Panel->ui->treeView);
}

void OrbitDataViewPanel::Refresh() { ui->treeView->Refresh(); }

void OrbitDataViewPanel::SetDataModel(DataView* model) { ui->treeView->SetDataModel(model); }

void OrbitDataViewPanel::SetFilter(const QString& a_Filter) {
  ui->FilterLineEdit->setText(a_Filter);
  ui->treeView->OnFilter(a_Filter);
}

void OrbitDataViewPanel::on_FilterLineEdit_textEdited(const QString& a_Text) {
  ui->treeView->OnFilter(a_Text);
}

void OrbitDataViewPanel::on_refreshButton_clicked() { ui->treeView->OnRefreshButtonClicked(); }
