#include <QGraphicsProxyWidget>
#include <QMessageBox>
#include <QFileDialog>
#include <QMenuBar>
#include <QTimer>
#include <functional>
#include "display/qt/subclasses/InteractibleNodeGraphNode_filter_graph_nodes.h"
#include "display/qt/subclasses/QGraphicsItem_interactible_node_graph_node.h"
#include "display/qt/subclasses/QGraphicsScene_interactible_node_graph.h"
#include "display/qt/dialogs/filter_graph_dialog.h"
#include "display/qt/widgets/filter_widgets.h"
#include "display/qt/persistent_settings.h"
#include "common/disk.h"
#include "ui_filter_graph_dialog.h"

FilterGraphDialog::FilterGraphDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::FilterGraphDialog)
{
    ui->setupUi(this);

    this->setWindowTitle(this->dialogBaseTitle);
    this->setWindowFlags(Qt::Window);

    // Create the dialog's menu bar.
    {
        this->menubar = new QMenuBar(this);

        // File...
        {
            QMenu *fileMenu = new QMenu("File", this);

            connect(fileMenu->addAction("New graph"), &QAction::triggered, this, [=]{this->reset_graph();});
            connect(fileMenu->addAction("Load graph..."), &QAction::triggered, this, [=]{this->load_filters();});
            fileMenu->addSeparator();
            connect(fileMenu->addAction("Save graph..."), &QAction::triggered, this, [=]{this->save_filters();});

            this->menubar->addMenu(fileMenu);
        }

        // Add...
        {
            QMenu *addMenu = new QMenu("Add", this);

            // Insert the names of all available filter types.
            {
                std::vector<const filter_meta_s*> knownFilterTypes = kf_known_filter_types();

                std::sort(knownFilterTypes.begin(), knownFilterTypes.end(), [](const filter_meta_s *a, const filter_meta_s *b)
                {
                    return a->name < b->name;
                });

                // Add gates.
                for (const auto filter: knownFilterTypes)
                {
                    if ((filter->type != filter_type_enum_e::output_gate) &&
                        (filter->type != filter_type_enum_e::input_gate))
                    {
                        continue;
                    }

                    connect(addMenu->addAction(QString::fromStdString(filter->name)), &QAction::triggered, this,
                            [=]{this->add_filter_node(filter->type);});
                }

                addMenu->addSeparator();

                // Add filters.
                for (const auto filter: knownFilterTypes)
                {
                    if ((filter->type == filter_type_enum_e::output_gate) ||
                        (filter->type == filter_type_enum_e::input_gate))
                    {
                        continue;
                    }

                    connect(addMenu->addAction(QString::fromStdString(filter->name)), &QAction::triggered, this,
                            [=]{this->add_filter_node(filter->type);});
                }
            }

            this->menubar->addMenu(addMenu);
        }

        // Help...
        {
            QMenu *helpMenu = new QMenu("Help", this);

            helpMenu->addAction("About...");
            helpMenu->actions().at(0)->setEnabled(false); /// TODO: Add the actual help.

            this->menubar->addMenu(helpMenu);
        }

        ui->widget_graphicsViewContainer->layout()->setMenuBar(menubar);
    }

    // Initialize the GUI controls to their default values.
    {
        ui->groupBox_filterGraphEnabled->setChecked(false);
        this->menubar->setEnabled(false); // Enabled if filtering is enabled.
    }

    // Create and configure the graphics scene.
    {
        this->graphicsScene = new InteractibleNodeGraph(this);
        this->graphicsScene->setBackgroundBrush(QBrush("#000000"));

        ui->graphicsView->setScene(this->graphicsScene);

        connect(this->graphicsScene, &InteractibleNodeGraph::edgeConnectionAdded, this, [this]{this->recalculate_filter_chains();});
        connect(this->graphicsScene, &InteractibleNodeGraph::edgeConnectionRemoved, this, [this]{this->recalculate_filter_chains();});
        connect(this->graphicsScene, &InteractibleNodeGraph::nodeRemoved, this, [this](InteractibleNodeGraphNode *const node)
        {
            FilterGraphNode *const filterNode = dynamic_cast<FilterGraphNode*>(node);

            if (filterNode)
            {
                if (filterNode->associatedFilter->metaData.type == filter_type_enum_e::input_gate)
                {
                    this->inputGateNodes.erase(std::find(inputGateNodes.begin(), inputGateNodes.end(), filterNode));
                }

                delete filterNode;

                /// TODO: When a node is deleted, recalculate_filter_chains() gets
                /// called quite a few times more than needed - once for each of its
                /// connections, and a last time here.
                this->recalculate_filter_chains();
            }
        });
    }

    // Connect the GUI controls to consequences for changing their values.
    {
        connect(ui->groupBox_filterGraphEnabled, &QGroupBox::toggled, this,
                [=](const bool isEnabled)
                {
                    kf_set_filtering_enabled(isEnabled);
                    kd_update_output_window_title();
                    this->menubar->setEnabled(isEnabled);
                });
    }

    // Restore persistent settings.
    {
        ui->groupBox_filterGraphEnabled->setChecked(kpers_value_of(INI_GROUP_OUTPUT, "custom_filtering", kf_is_filtering_enabled()).toBool());
        this->resize(kpers_value_of(INI_GROUP_GEOMETRY, "filter_graph", this->size()).toSize());
    }

    this->reset_graph(true);

    return;
}

FilterGraphDialog::~FilterGraphDialog()
{
    // Save persistent settings.
    {
        kpers_set_value(INI_GROUP_OUTPUT, "custom_filtering", ui->groupBox_filterGraphEnabled->isChecked());
        kpers_set_value(INI_GROUP_GEOMETRY, "filter_graph", this->size());
    }

    delete ui;

    return;
}

void FilterGraphDialog::reset_graph(const bool autoAccept)
{
    if (autoAccept ||
        QMessageBox::question(this,
                              "Create a new graph?",
                              "Any unsaved changes in the current graph will be lost. Continue?") == QMessageBox::Yes)
    {
        this->clear_filter_graph();
    }

    return;
}

void FilterGraphDialog::load_filters(void)
{
    QString filename = QFileDialog::getOpenFileName(this,
                                                    "Select a file containing the filter graph to be loaded", "",
                                                    "Filter graphs (*.vcs-filter-graph);;Legacy filter sets (*.vcs-filtersets);;"
                                                    "All files(*.*)");

    if (filename.isEmpty())
    {
        return;
    }

    kdisk_load_filter_graph(filename.toStdString());

    return;
}

void FilterGraphDialog::save_filters(void)
{
    QString filename = QFileDialog::getSaveFileName(this,
                                                    "Select a file to save the filter graph into", "",
                                                    "Filter files (*.vcs-filter-graph);;"
                                                    "All files(*.*)");

    if (filename.isEmpty())
    {
        return;
    }

    if (QFileInfo(filename).suffix().isEmpty())
    {
        filename += ".vcs-filter-graph";
    }

    std::vector<FilterGraphNode*> filterNodes;
    {
        const QList<QGraphicsItem*> sceneNodes = this->graphicsScene->items();
        for (auto node: sceneNodes)
        {
            const auto filterNode = dynamic_cast<FilterGraphNode*>(node);

            if (filterNode)
            {
                filterNodes.push_back(filterNode);
            }
        }
    }

    std::vector<filter_graph_option_s> graphOptions;
    {
        /// TODO.
    }

    kdisk_save_filter_graph(filterNodes, graphOptions, filename);

    return;
}

// Adds a new instance of the given filter type into the node graph. Returns a
// pointer to the new node.
FilterGraphNode* FilterGraphDialog::add_filter_node(const filter_type_enum_e type,
                                                    const u8 *const initialParameterValues)
{
    const filter_c *const newFilter = kf_create_new_filter_instance(type, initialParameterValues);
    const unsigned filterWidgetWidth = (newFilter->guiWidget->widget->width() + 20);
    const unsigned filterWidgetHeight = (newFilter->guiWidget->widget->height() + 49);
    const QString nodeTitle = QString("#%1: %2").arg(this->numNodesAdded+1).arg(newFilter->guiWidget->title);

    k_assert(newFilter, "Failed to add a new filter node.");

    FilterGraphNode *newNode = nullptr;

    switch (type)
    {
        case filter_type_enum_e::input_gate: newNode = new InputGateNode(nodeTitle, filterWidgetWidth, filterWidgetHeight); break;
        case filter_type_enum_e::output_gate: newNode = new OutputGateNode(nodeTitle, filterWidgetWidth, filterWidgetHeight); break;
        default: newNode = new FilterNode(nodeTitle, filterWidgetWidth, filterWidgetHeight); break;
    }

    newNode->associatedFilter = newFilter;
    newNode->setFlags(newNode->flags() | QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsSelectable);
    this->graphicsScene->addItem(newNode);

    QGraphicsProxyWidget* nodeWidgetProxy = new QGraphicsProxyWidget(newNode);
    nodeWidgetProxy->setWidget(newFilter->guiWidget->widget);
    nodeWidgetProxy->widget()->move(10, 40);

    if (type == filter_type_enum_e::input_gate)
    {
        this->inputGateNodes.push_back(newNode);
    }

    newNode->moveBy(rand()%20, rand()%20);

    real maxZ = 0;
    const auto sceneItems = this->graphicsScene->items();
    for (auto item: sceneItems)
    {
        if (item->zValue() > maxZ)
        {
            maxZ = item->zValue();
        }
    }
    newNode->setZValue(maxZ+1);

    ui->graphicsView->centerOn(newNode);

    this->numNodesAdded++;

    return newNode;
}

// Visit each node in the graph and while doing so, group together such chains of
// filters that run from an input gate through one or more filters into an output
// gate. The chains will then be submitted to the filter handler for use in applying
// the filters to captured frames.
void FilterGraphDialog::recalculate_filter_chains(void)
{
    kf_remove_all_filter_chains();

    const std::function<void(FilterGraphNode *const, std::vector<const filter_c*>)> traverse_filter_node =
          [&](FilterGraphNode *const node, std::vector<const filter_c*> accumulatedFilterChain)
    {
        k_assert((node && node->associatedFilter), "Trying to visit an invalid node.");

        if (std::find(accumulatedFilterChain.begin(),
                      accumulatedFilterChain.end(),
                      node->associatedFilter) != accumulatedFilterChain.end())
        {
            kd_show_headless_error_message("VCS detected a potential infinite loop",
                                           "One or more filter chains in the filter graph are connected in a loop "
                                           "(a node's output connects back to its input).\n\nChains containing an "
                                           "infinite loop will remain unusable until the loop is disconnected.");

            return;
        }

        accumulatedFilterChain.push_back(node->associatedFilter);

        if (node->associatedFilter->metaData.type == filter_type_enum_e::output_gate)
        {
            kf_add_filter_chain(accumulatedFilterChain);
            return;
        }

        // NOTE: This assumes that each node in the graph only has one output edge.
        for (auto outgoing: node->output_edge()->connectedTo)
        {
            traverse_filter_node(dynamic_cast<FilterGraphNode*>(outgoing->parentNode), accumulatedFilterChain);
        }

        return;
    };

    for (auto inputGate: this->inputGateNodes)
    {
        traverse_filter_node(inputGate, {});
    }

    return;
}

void FilterGraphDialog::clear_filter_graph(void)
{
    kf_remove_all_filter_chains();
    this->graphicsScene->reset_scene();
    this->inputGateNodes.clear();
    this->numNodesAdded = 0;

    this->setWindowTitle(QString("%1 - %2").arg(this->dialogBaseTitle).arg("Unsaved graph"));

    return;
}

void FilterGraphDialog::refresh_filter_graph(void)
{
    this->graphicsScene->update_scene_connections();

    return;
}


void FilterGraphDialog::set_filter_graph_source_filename(const std::string &sourceFilename)
{
    const QString baseFilename = QFileInfo(sourceFilename.c_str()).fileName();

    this->setWindowTitle(QString("%1 - %2").arg(this->dialogBaseTitle).arg(baseFilename));

    // Kludge fix for the filter graph not repainting itself properly when new nodes
    // are loaded in. Let's just force it to do so.
    this->refresh_filter_graph();

    return;
}

void FilterGraphDialog::set_filter_graph_options(const std::vector<filter_graph_option_s> &graphOptions)
{
    for (const filter_graph_option_s &option: graphOptions)
    {
        /// TODO.

        (void)option;
    }

    return;
}


FilterGraphNode* FilterGraphDialog::add_filter_graph_node(const filter_type_enum_e &filterType,
                                                          const u8 *const initialParameterValues)
{
    return add_filter_node(filterType, initialParameterValues);
}

void FilterGraphDialog::toggle_filtering(void)
{
    ui->groupBox_filterGraphEnabled->setChecked(!ui->groupBox_filterGraphEnabled->isChecked());

    return;
}
