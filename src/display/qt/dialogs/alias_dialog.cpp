/*
 * 2018 Tarpeeksi Hyvae Soft /
 * VCS alias resolutions dialog
 *
 */

#include <QTreeWidgetItem>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QMenuBar>
#include <QSpinBox>
#include <QDebug>
#include <QLabel>
#include "display/qt/dialogs/alias_dialog.h"
#include "display/qt/persistent_settings.h"
#include "display/display.h"
#include "capture/capture.h"
#include "capture/alias.h"
#include "common/disk.h"
#include "ui_alias_dialog.h"

// Custom Qt::ItemDataRole values.
enum data_role
{
    // An alias's source/target resolution.
    width = 0x101,
    height = 0x102,
};

AliasDialog::AliasDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::AliasDialog)
{
    ui->setupUi(this);

    this->setWindowTitle("VCS - Alias Resolutions");

    // Don't show the context help '?' button in the window bar.
    this->setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    // Create the dialog's menu bar.
    {
        this->menubar = new QMenuBar(this);

        // File...
        {
            QMenu *fileMenu = new QMenu("File", this);

            fileMenu->addAction("Load aliases...", this, SLOT(load_aliases()));
            fileMenu->addSeparator();
            fileMenu->addAction("Save aliases as...", this, SLOT(save_aliases()));

            menubar->addMenu(fileMenu);
        }

        // Aliases...
        {
            QMenu *fileMenu = new QMenu("Aliases", this);

            fileMenu->addAction("Add an alias", this, SLOT(add_alias()));
            fileMenu->addSeparator();
            fileMenu->addAction("Remove selected", this, SLOT(remove_selected_aliases()));

            menubar->addMenu(fileMenu);
        }

        // Help...
        {
            QMenu *fileMenu = new QMenu("Help", this);

            fileMenu->addAction("About...");
            fileMenu->actions().at(0)->setEnabled(false); /// TODO: Add proper help stuff.

            menubar->addMenu(fileMenu);
        }

        this->layout()->setMenuBar(menubar);
    }

    // Restore persistent settings.
    {
        this->resize(kpers_value_of(INI_GROUP_GEOMETRY, "aliases", this->size()).toSize());
    }

    return;
}

AliasDialog::~AliasDialog()
{
    // Save persistent settings.
    {
        kpers_set_value(INI_GROUP_GEOMETRY, "aliases", this->size());
    }

    delete ui;
    ui = nullptr;

    return;
}

// Send the user-defined alias resolutions from the GUI to the capturer,
// which puts them into use.
void AliasDialog::broadcast_aliases(void)
{
    std::vector<mode_alias_s> aliases;

    auto items = ui->treeWidget_knownAliases->findItems("*", Qt::MatchWildcard);
    for (auto *item: items)
    {
        mode_alias_s a;

        a.from.w = item->data(0, data_role::width).toUInt();
        a.from.h = item->data(0, data_role::height).toUInt();
        a.to.w = item->data(1, data_role::width).toUInt();
        a.to.h = item->data(1, data_role::height).toUInt();

        aliases.push_back(a);
    }

    ka_set_aliases(aliases);

    return;
}

void AliasDialog::adjust_treewidget_column_size(void)
{
    ui->treeWidget_knownAliases->setColumnWidth(0, (ui->treeWidget_knownAliases->width() / 2));

    return;
}

void AliasDialog::resizeEvent(QResizeEvent *)
{
    this->adjust_treewidget_column_size();

    return;
}

// Add the given alias into the GUI.
void AliasDialog::receive_new_alias(const mode_alias_s a)
{
    // Returns a widget containing two QSpinBoxes, corresponding to a resolution's
    // width and height. In other words, with the widget, the user can specify a resolution.
    // This widget is meant to be embedded into a column on a QTreeWidgetItem; the
    // spinboxes' values are stored in the tree item with custom data roles.
    auto create_resolution_widget = [this](QTreeWidgetItem *parentItem,
                                           const uint column,
                                           const uint width,
                                           const uint height)->QWidget*
    {
        QWidget *container = new QWidget;
        QHBoxLayout *layout = new QHBoxLayout;
        container->setLayout(layout);

        QSpinBox *x = new QSpinBox;
        QSpinBox *y = new QSpinBox;

        x->setMinimum(1);
        y->setMinimum(1);

        x->setMaximum(MAX_OUTPUT_WIDTH);
        y->setMaximum(MAX_OUTPUT_HEIGHT);

        x->setButtonSymbols(QAbstractSpinBox::NoButtons);
        y->setButtonSymbols(QAbstractSpinBox::NoButtons);

        x->setValue(width);
        y->setValue(height);

        parentItem->setData(column, data_role::width, x->value());
        parentItem->setData(column, data_role::height, y->value());

        connect(x, &QSpinBox::editingFinished, [this, parentItem, x, column]
        {
            parentItem->setData(column, data_role::width, x->value());
            this->broadcast_aliases();
        });

        connect(y, &QSpinBox::editingFinished, [this, parentItem, y, column]
        {
            parentItem->setData(column, data_role::height, y->value());
            this->broadcast_aliases();
        });

        layout->addWidget(x);
        layout->addWidget(y);

        return container;
    };

    QTreeWidgetItem *entry = new QTreeWidgetItem;
    ui->treeWidget_knownAliases->addTopLevelItem(entry);
    ui->treeWidget_knownAliases->setItemWidget(entry, 0, create_resolution_widget(entry, 0, a.from.w, a.from.h));
    ui->treeWidget_knownAliases->setItemWidget(entry, 1, create_resolution_widget(entry, 1, a.to.w, a.to.h));

    this->adjust_treewidget_column_size();

    return;
}

void AliasDialog::clear_known_aliases()
{
    ui->treeWidget_knownAliases->clear();

    return;
}

void AliasDialog::add_alias(void)
{
    mode_alias_s newAlias;
    newAlias.from = {1, 1, 0};
    newAlias.to = {640, 480, 0};

    this->receive_new_alias(newAlias);

    this->broadcast_aliases();

    return;
}

void AliasDialog::remove_selected_aliases(void)
{
    const auto selectedItems = ui->treeWidget_knownAliases->selectedItems();
    for (auto item: selectedItems)
    {
        delete item;
    }

    this->broadcast_aliases();

    return;
}

void AliasDialog::load_aliases()
{
    QString filename = QFileDialog::getOpenFileName(this,
                                                    "Select the file to load aliases from", "",
                                                    "Alias files (*.vcsa);;"
                                                    "All files(*.*)");

    if (filename.isEmpty())
    {
        return;
    }

    kdisk_load_aliases(filename.toStdString());

    return;
}

void AliasDialog::save_aliases()
{
    QString filename = QFileDialog::getSaveFileName(this,
                                                    "Select the file to save the aliases into", "",
                                                    "Alias files (*.vcsa);;"
                                                    "All files(*.*)");

    if (filename.isEmpty())
    {
        return;
    }

    if (QFileInfo(filename).suffix().isEmpty())
    {
        filename += ".vcsa";
    }

    this->broadcast_aliases();

    kdisk_save_aliases(ka_aliases(), filename);

    return;
}
