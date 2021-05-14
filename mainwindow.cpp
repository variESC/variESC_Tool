/*
    Copyright 2016 - 2020 Benjamin Vedder	benjamin@vedder.se

    This file is part of VESC Tool.

    VESC Tool is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    VESC Tool is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
    */

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QMessageBox>
#include <QDebug>
#include <QFileDialog>
#include <QListWidgetItem>
#include <cmath>
#include <QEventLoop>
#include <QDesktopServices>
#include <QProgressDialog>
#include "parametereditor.h"
#include "startupwizard.h"
#include "widgets/helpdialog.h"
#include "utility.h"
#include "widgets/paramdialog.h"
#include "widgets/detectallfocdialog.h"

namespace {
void stepTowards(double &value, double goal, double step) {
    if (value < goal) {
        if ((value + step) < goal) {
            value += step;
        } else {
            value = goal;
        }
    } else if (value > goal) {
        if ((value - step) > goal) {
            value -= step;
        } else {
            value = goal;
        }
    }
}

void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    QByteArray localMsg = msg.toLocal8Bit();
    QString str;
    QString typeStr;
    bool isBad = false;

    switch (type) {
    case QtDebugMsg: typeStr = "DEBUG"; break;
        //    case QtInfoMsg: typeStr = "INFO"; break;
    case QtWarningMsg: typeStr = "WARNING"; isBad = true; break;
    case QtCriticalMsg: typeStr = "CRITICAL"; isBad = true; break;
    case QtFatalMsg: typeStr = "FATAL"; isBad = true; break;

    default:
        break;
    }

    str.sprintf("%s (%s:%u %s): %s", typeStr.toLocal8Bit().data(),
                context.file, context.line, context.function, localMsg.constData());

    if (PageDebugPrint::currentMsgHandler) {
        QString strTmp;
        if (isBad) {
            strTmp = "<font color=\"red\">" + str + "</font><br>";
        } else {
            strTmp = str + "<br>";
        }

        QMetaObject::invokeMethod(PageDebugPrint::currentMsgHandler, "printConsole",
                                  Qt::QueuedConnection, Q_ARG(QString, strTmp));
    }

    printf("%s\n", str.toLocal8Bit().data());
    fflush(stdout);
}
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    mVersion = QString::number(VT_VERSION, 'f', 2);
    mVesc = new VescInterface(this);
    mStatusInfoTime = 0;
    mStatusLabel = new QLabel(this);
    ui->statusBar->addPermanentWidget(mStatusLabel);
    mTimer = new QTimer(this);
    mKeyLeft = false;
    mKeyRight = false;

    connect(mTimer, SIGNAL(timeout()),
            this, SLOT(timerSlot()));
    connect(mVesc, SIGNAL(statusMessage(QString,bool)),
            this, SLOT(showStatusInfo(QString,bool)));
    connect(mVesc, SIGNAL(messageDialog(QString,QString,bool,bool)),
            this, SLOT(showMessageDialog(QString,QString,bool,bool)));
    connect(mVesc, SIGNAL(serialPortNotWritable(QString)),
            this, SLOT(serialPortNotWritable(QString)));
    connect(mVesc->commands(), SIGNAL(valuesReceived(MC_VALUES,unsigned int)),
            this, SLOT(valuesReceived(MC_VALUES,unsigned int)));
    connect(mVesc->commands(), SIGNAL(mcConfigCheckResult(QStringList)),
            this, SLOT(mcConfigCheckResult(QStringList)));
    connect(mVesc->mcConfig(), SIGNAL(paramChangedDouble(QObject*,QString,double)),
            this, SLOT(paramChangedDouble(QObject*,QString,double)));
    connect(ui->actionAboutQt, SIGNAL(triggered(bool)),
            qApp, SLOT(aboutQt()));
    connect(mVesc->commands(), SIGNAL(pingCanRx(QVector<int>,bool)),
            this, SLOT(pingCanRx(QVector<int>,bool)));

    ui->dispDuty->setName("Duty");
    ui->dispDuty->setRange(100.0);
    ui->dispDuty->setUnit(" %");
    ui->dispDuty->setDecimals(1);

    // Remove the menu with the option to hide the toolbar
    ui->mainToolBar->setContextMenuPolicy(Qt::PreventContextMenu);

    mVesc->fwConfig()->loadParamsXml("://res/config/fw.xml");
    Utility::configLoadLatest(mVesc);

    QMenu *fwMenu = new QMenu(this);
    fwMenu->setTitle("Load Firmware Configs");
    fwMenu->setIcon(QIcon("://res/icons/Electronics-96.png"));
    for (auto fw: Utility::configSupportedFws()) {
        QAction *action = new QAction(fwMenu);
        action->setText(QString("%1.%2").arg(fw.first).arg(fw.second, 2, 10, QChar('0')));
        connect(action, &QAction::triggered, [this,fw]() {
            Utility::configLoad(mVesc, fw.first, fw.second);
        });
        fwMenu->addAction(action);
    }
    ui->menuTools->addMenu(fwMenu);

    QMenu *backupMenu = new QMenu(this);

    auto reloadBackupMenu = [this, backupMenu]() {
        backupMenu->clear();
        backupMenu->setTitle("Load Configuration Backups for UUID");
        backupMenu->setIcon(QIcon("://res/icons/Open Folder-96.png"));
        for (auto uuid: mVesc->confListBackups()) {
            QAction *action = new QAction(backupMenu);
            action->setIcon(QIcon("://res/icons/Electronics-96.png"));
            QString txt = uuid;
            QString name = mVesc->confBackupName(uuid);
            if (!name.isEmpty()) {
                txt += " (" + name + ")";
            }
            action->setText(txt);
            connect(action, &QAction::triggered, [this,uuid]() {
                mVesc->confLoadBackup(uuid);
            });
            backupMenu->addAction(action);
        }
        ui->menuTools_2->addMenu(backupMenu);
    };

    reloadBackupMenu();
    connect(mVesc, &VescInterface::configurationBackupsChanged, reloadBackupMenu);

    reloadPages();

    mMcConfRead = false;
    mAppConfRead = false;

    connect(mVesc->mcConfig(), &ConfigParams::updated, [this]() {
        mMcConfRead = true;
    });
    connect(mVesc->appConfig(), &ConfigParams::updated, [this]() {
        mAppConfRead = true;
    });

    auto updateMotortype = [this]() {
        if (mVesc->getLastFwRxParams().hwType == HW_TYPE_VESC) {
            int type = mVesc->mcConfig()->getParamEnum("motor_type");
            ui->pageList->item(mPageNameIdList.value("motor_bldc"))->setHidden(type != 0);
            ui->pageList->item(mPageNameIdList.value("motor_dc"))->setHidden(type != 1);
            ui->pageList->item(mPageNameIdList.value("motor_foc"))->setHidden(type != 2);
            ui->pageList->item(mPageNameIdList.value("motor_gpdrive"))->setHidden(type != 3);
        }
    };

    auto updateAppToUse = [this]() {
        if (mVesc->getLastFwRxParams().hwType == HW_TYPE_VESC) {
            int type = mVesc->appConfig()->getParamEnum("app_to_use");
            ui->pageList->item(mPageNameIdList.value("app_ppm"))->setHidden
                    (!(type == 1 || type == 4 || type == 8));
            ui->pageList->item(mPageNameIdList.value("app_adc"))->setHidden
                    (!(type == 2 || type == 5 || type == 8 || type == 11));
            ui->pageList->item(mPageNameIdList.value("app_uart"))->setHidden
                    (!(type == 3 || type == 4 || type == 5 || type == 8));
            ui->pageList->item(mPageNameIdList.value("app_vescremote"))->setHidden
                    (!(type == 0 || type == 3 || type == 6 || type == 7 || type == 8));
            ui->pageList->item(mPageNameIdList.value("app_balance"))->setHidden
                    (!(type == 8 || type == 9));
            ui->pageList->item(mPageNameIdList.value("app_pas"))->setHidden
                    (!(type == 10 || type == 11));
        }
    };

    connect(mVesc, &VescInterface::fwRxChanged,
            [this, updateMotortype, updateAppToUse](bool rx, bool limited) {
        (void)limited;

        if (!rx) {
            return;
        }

        FW_RX_PARAMS params = mVesc->getLastFwRxParams();

        if (params.hwType == HW_TYPE_VESC) {
            ui->pageList->item(mPageNameIdList.value("motor"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("motor_general"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("motor_bldc"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("motor_dc"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("motor_foc"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("motor_gpdrive"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("motor_pid"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("motor_additional_info"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("motor_experiments"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("app"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("app_general"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("app_ppm"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("app_adc"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("app_uart"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("app_vescremote"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("app_nrf"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("app_balance"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("app_pas"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("app_imu"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("data_rt"))->setHidden(false);
            ui->pageList->item(mPageNameIdList.value("data_sampled"))->setHidden(false);

            QTimer::singleShot(100, [updateMotortype,updateAppToUse]() {
                updateMotortype();
                updateAppToUse();
            });
        } else {
            ui->pageList->item(mPageNameIdList.value("motor"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("motor_general"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("motor_bldc"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("motor_dc"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("motor_foc"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("motor_gpdrive"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("motor_pid"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("motor_additional_info"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("motor_experiments"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("app"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("app_general"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("app_ppm"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("app_adc"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("app_uart"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("app_vescremote"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("app_nrf"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("app_balance"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("app_pas"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("app_imu"))->setHidden(true);
//            ui->pageList->item(mPageNameIdList.value("data_rt"))->setHidden(true);
            ui->pageList->item(mPageNameIdList.value("data_sampled"))->setHidden(true);
        }
    });

    connect(mVesc, &VescInterface::configurationChanged, [updateMotortype, updateAppToUse, this]() {
        qDebug() << "Reloading user interface due to configuration change.";

        mMcConfRead = false;
        mAppConfRead = false;

        mPageMotorSettings->reloadParams();
        mPageMotor->reloadParams();
        mPageBldc->reloadParams();
        mPageDc->reloadParams();
        mPageFoc->reloadParams();
        mPageGpd->reloadParams();
        mPageControllers->reloadParams();
        mPageMotorInfo->reloadParams();
        mPageAppSettings->reloadParams();
        mPageAppGeneral->reloadParams();
        mPageAppPpm->reloadParams();
        mPageAppAdc->reloadParams();
        mPageAppUart->reloadParams();
        mPageAppNunchuk->reloadParams();
        mPageAppNrf->reloadParams();
        mPageAppBalance->reloadParams();
        mPageAppPas->reloadParams();
        mPageAppImu->reloadParams();
        mPageFirmware->reloadParams();
        mPageCanAnalyzer->reloadParams();

        updateMotortype();
        updateAppToUse();
    });

    connect(mVesc->mcConfig(), &ConfigParams::paramChangedEnum,
            [updateMotortype](QObject *src, QString name, int newParam) {
        (void)src;
        (void)newParam;
        if (name == "motor_type") {
            updateMotortype();
        }
    });

    connect(mVesc->appConfig(), &ConfigParams::paramChangedEnum,
            [updateAppToUse](QObject *src, QString name, int newParam) {
        (void)src;
        (void)newParam;
        if (name == "app_to_use") {
            updateAppToUse();
        }
    });

    connect(mVesc, &VescInterface::customConfigLoadDone, [this]() {
        ui->pageList->item(mPageNameIdList.value("app_custom_config_0"))->setHidden(mVesc->customConfigNum() < 1);
        ui->pageList->item(mPageNameIdList.value("app_custom_config_1"))->setHidden(mVesc->customConfigNum() < 2);
        ui->pageList->item(mPageNameIdList.value("app_custom_config_2"))->setHidden(mVesc->customConfigNum() < 3);

        if (mVesc->customConfig(0)) {
            auto item = ui->pageList->item(mPageNameIdList.value("app_custom_config_0"));
            PageListItem *widget = dynamic_cast<PageListItem*>(ui->pageList->itemWidget(item));
            widget->setName(mVesc->customConfig(0)->getParam("hw_name")->longName);
        }

        if (mVesc->customConfig(1)) {
            auto item = ui->pageList->item(mPageNameIdList.value("app_custom_config_1"));
            PageListItem *widget = dynamic_cast<PageListItem*>(ui->pageList->itemWidget(item));
            widget->setName(mVesc->customConfig(1)->getParam("hw_name")->longName);
        }

        if (mVesc->customConfig(2)) {
            auto item = ui->pageList->item(mPageNameIdList.value("app_custom_config_2"));
            PageListItem *widget = dynamic_cast<PageListItem*>(ui->pageList->itemWidget(item));
            widget->setName(mVesc->customConfig(2)->getParam("hw_name")->longName);
        }
    });

    qApp->installEventFilter(this);
    qInstallMessageHandler(myMessageOutput);

    mTimer->start(20);

    // Restore size and position
    if (mSettings.contains("mainwindow/size")) {
        resize(mSettings.value("mainwindow/size").toSize());
    }

    if (mSettings.contains("mainwindow/position")) {
        move(mSettings.value("mainwindow/position").toPoint());
    }

    if (mSettings.contains("mainwindow/maximized")) {
        bool maximized = mSettings.value("mainwindow/maximized").toBool();
        if (maximized) {
            showMaximized();
        }
    }

    updateMotortype();
    updateAppToUse();

    ui->leftSplitter->setSizes(QList<int>({1000, 80}));

    mPageDebugPrint->printConsole("variESC Tool " + mVersion + " started<br>");
}

MainWindow::~MainWindow()
{
    // Save settings
    mSettings.setValue("version", mVersion);
    mSettings.setValue("introVersion", VT_INTRO_VERSION);
    mSettings.setValue("mainwindow/position", pos());
    mSettings.setValue("mainwindow/maximized", isMaximized());

    if (!isMaximized()) {
        mSettings.setValue("mainwindow/size", size());
    }

    delete ui;
}

bool MainWindow::eventFilter(QObject *object, QEvent *e)
{
    (void)object;

    if (!mVesc->isPortConnected() || !ui->actionKeyboardControl->isChecked()) {
        return false;
    }

    if (e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(e);
        bool isPress = e->type() == QEvent::KeyPress;

        switch(keyEvent->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_PageDown:
            break;

        default:
            return false;
        }

        if(keyEvent->isAutoRepeat()) {
            return true;
        }

        switch(keyEvent->key()) {
        case Qt::Key_Up:
            if (isPress) {
                mVesc->commands()->setCurrent(ui->currentBox->value());
                ui->actionSendAlive->setChecked(true);
            } else {
                mVesc->commands()->setCurrent(0.0);
                ui->actionSendAlive->setChecked(false);
            }
            break;

        case Qt::Key_Down:
            if (isPress) {
                mVesc->commands()->setCurrent(-ui->currentBox->value());
                ui->actionSendAlive->setChecked(true);
            } else {
                mVesc->commands()->setCurrent(0.0);
                ui->actionSendAlive->setChecked(false);
            }
            break;

        case Qt::Key_Left:
            if (isPress) {
                mKeyLeft = true;
            } else {
                mKeyLeft = false;
            }
            break;

        case Qt::Key_Right:
            if (isPress) {
                mKeyRight = true;
            } else {
                mKeyRight = false;
            }
            break;

        case Qt::Key_PageDown:
            if (isPress) {
                mVesc->commands()->setCurrentBrake(-ui->currentBox->value());
                ui->actionSendAlive->setChecked(true);
            } else {
                mVesc->commands()->setCurrent(0.0);
                ui->actionSendAlive->setChecked(false);
            }
            break;

        default:
            break;
        }

        return true;
    }

    return false;
}

void MainWindow::timerSlot()
{
    // Update status label
    if (mStatusInfoTime) {
        mStatusInfoTime--;
        if (!mStatusInfoTime) {
            mStatusLabel->setStyleSheet(qApp->styleSheet());
        }
    } else {
        QString str = mVesc->getConnectedPortName();
        if (str != mStatusLabel->text()) {
            mStatusLabel->setText(mVesc->getConnectedPortName());
            static QString statusLast = "";
            if (str != statusLast) {
                mPageDebugPrint->printConsole("Status: " + str + "<br>");
                statusLast = str;
            }
        }
    }

    // Scan can bus on connect
    if (mVesc->isPortConnected() && ui->canList->count() == 0 && ui->scanCanButton->isEnabled() && mVesc->fwRx()) {
        on_scanCanButton_clicked();
    }

    // If disconnected for a short time clear the can list so it scans on reconnect.
    // Also disable CAN fwd for newer users who try to reconnect to non-existent CAN device from different setup.
    static int disconected_cnt = 0;
    disconected_cnt++;
    if (disconected_cnt >= 20 && ui->canList->count() > 0) {
        ui->canList->clear();
        mVesc->commands()->setSendCan(false);
        ui->scanCanButton->setEnabled(true);
        ui->pageList->item(mPageNameIdList.value("app_custom_config_0"))->setHidden(true);
        ui->pageList->item(mPageNameIdList.value("app_custom_config_1"))->setHidden(true);
        ui->pageList->item(mPageNameIdList.value("app_custom_config_2"))->setHidden(true);
    }

    if (disconected_cnt >= 20 && !mVesc->isPortConnected()) {
        ui->scanCanButton->setEnabled(true);
    }

    if (!mVesc->isIgnoringCanChanges()) {
        // CAN fwd
        if (ui->actionCanFwd->isChecked() != mVesc->commands()->getSendCan()) {
            ui->actionCanFwd->setChecked(mVesc->commands()->getSendCan());
        }

        if (mVesc->commands()->getSendCan()) {
            int id_set = mVesc->commands()->getCanSendId();
            for (int i = 1; i <  ui->canList->count(); i++) {
                int id_ui = ui->canList->item(i)->text().split(" ").last().toInt();
                if (id_ui == id_set) {
                    ui->canList->setCurrentRow(i);
                    break;
                }
            }
        } else {
            ui->canList->setCurrentRow(0);
        }
    }

    // RT data
    if (ui->actionRtData->isChecked()) {
        mVesc->commands()->getValues();
        if (mVesc->isRtLogOpen()) {
            mVesc->commands()->getValuesSetup();
        }
    }

    // APP RT data
    if (ui->actionRtDataApp->isChecked()) {
        mVesc->commands()->getDecodedAdc();
        mVesc->commands()->getDecodedChuk();
        mVesc->commands()->getDecodedPpm();
        mVesc->commands()->getDecodedBalance();
    }

    // IMU Data
    if (ui->actionIMU->isChecked()) {
        mVesc->commands()->getImuData(0xFFFF);
    }

    // BMS Data
    if (ui->actionrtDataBms->isChecked()) {
        mVesc->commands()->bmsGetValues();
    }

    // Send alive command once every 10 iterations
    if (ui->actionSendAlive->isChecked()) {
        static int alive_cnt = 0;
        alive_cnt++;
        if (alive_cnt >= 10) {
            alive_cnt = 0;
            mVesc->commands()->sendAlive();
        }
    }

    // Read configurations if they haven't been read since starting VESC Tool
    if (mVesc->isPortConnected() && mVesc->fwRx()) {
        static int conf_cnt = 0;
        disconected_cnt = 0;
        conf_cnt++;
        if (conf_cnt >= 20) {
            conf_cnt = 0;

            if (!mVesc->deserializeFailedSinceConnected() && mVesc->fwRx() &&
                    mVesc->getLastFwRxParams().hwType == HW_TYPE_VESC) {
                if (!mMcConfRead) {
                    mVesc->commands()->getMcconf();
                }

                if (!mAppConfRead) {
                    mVesc->commands()->getAppConf();
                }
            }
        }
    }

    // Disable all data streaming when uploading firmware
    if (mVesc->getFwUploadProgress() > 0.1) {
        ui->actionSendAlive->setChecked(false);
        ui->actionRtData->setChecked(false);
        ui->actionRtDataApp->setChecked(false);
        ui->actionIMU->setChecked(false);
        ui->actionKeyboardControl->setChecked(false);
        ui->actionrtDataBms->setChecked(false);
    }

    // Handle key events
    static double keyPower = 0.0;
    static double lastKeyPower = 0.0;
    const double lowPower = 0.18;
    const double lowPowerRev = 0.1;
    const double highPower = 0.9;
    const double highPowerRev = 0.3;
    const double lowStep = 0.02;
    const double highStep = 0.01;

    if (mKeyRight && mKeyLeft) {
        if (keyPower >= lowPower) {
            stepTowards(keyPower, highPower, highStep);
        } else if (keyPower <= -lowPower) {
            stepTowards(keyPower, -highPowerRev, highStep);
        } else if (keyPower >= 0) {
            stepTowards(keyPower, highPower, lowStep);
        } else {
            stepTowards(keyPower, -highPowerRev, lowStep);
        }
    } else if (mKeyRight) {
        if (fabs(keyPower) > lowPower) {
            stepTowards(keyPower, lowPower, highStep);
        } else {
            stepTowards(keyPower, lowPower, lowStep);
        }
    } else if (mKeyLeft) {
        if (fabs(keyPower) > lowPower) {
            stepTowards(keyPower, -lowPowerRev, highStep);
        } else {
            stepTowards(keyPower, -lowPowerRev, lowStep);
        }
    } else {
        stepTowards(keyPower, 0.0, lowStep * 3);
    }

    if (keyPower != lastKeyPower) {
        lastKeyPower = keyPower;
        mVesc->commands()->setDutyCycle(keyPower);
        ui->actionSendAlive->setChecked(true);
    }

    // Run startup checks
    static bool has_run_start_checks = false;
    if (!has_run_start_checks) {
        if (mSettings.contains("introVersion")) {
            if (mSettings.value("introVersion").toInt() != VT_INTRO_VERSION) {
                mSettings.setValue("intro_done", false);
            }
        } else {
            mSettings.setValue("intro_done", false);
        }

        if (!mSettings.contains("intro_done")) {
            mSettings.setValue("intro_done", false);
        }

        if (!mSettings.value("intro_done").toBool()) {
            StartupWizard w(mVesc, this);
            w.exec();
        }

        if (!mSettings.value("intro_done").toBool()) {
            QMessageBox::critical(this,
                                  tr("Warning"),
                                  tr("You have not finished the variESC Tool introduction. You must do that "
                                     "in order to use variESC Tool."));
            QCoreApplication::quit();
        }

        has_run_start_checks = true;
        checkUdev();
    }
}

void MainWindow::showStatusInfo(QString info, bool isGood)
{
    if (isGood) {
        mStatusLabel->setStyleSheet("QLabel { background-color : lightgreen; color : black; }");
        mPageDebugPrint->printConsole("Status: " + info + "<br>");
    } else {
        mStatusLabel->setStyleSheet("QLabel { background-color : red; color : black; }");
        mPageDebugPrint->printConsole("<font color=\"red\">Status: " + info + "</font><br>");
    }

    mStatusInfoTime = (80 * 20) / mTimer->interval();
    mStatusLabel->setText(info);
}

void MainWindow::showMessageDialog(const QString &title, const QString &msg, bool isGood, bool richText)
{
    (void)richText;

    if (isGood) {
        QMessageBox::information(this, title, msg);
    } else {
        QMessageBox::warning(this, title, msg);
    }
}

void MainWindow::serialPortNotWritable(const QString &port)
{
    (void)port;

#ifdef Q_OS_LINUX
    QMessageBox::StandardButton reply;
    reply = QMessageBox::warning(this,
                                 tr("Connect Serial Port"),
                                 tr("The serial port is not writable. This can usually be fixed by "
                                    "adding your user to the dialout, uucp and/or lock groups. Would "
                                    "you like to do that?"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (reply == QMessageBox::Yes) {
        QString name = qgetenv("USER");
        if (name.isEmpty()) {
            name = qgetenv("USERNAME");
        }

        bool hasDialout = !runCmd("getent", QStringList() << "group" << "dialout").isEmpty();
        bool hasUucp = !runCmd("getent", QStringList() << "group" << "uucp").isEmpty();
        bool hasLock = !runCmd("getent", QStringList() << "group" << "lock").isEmpty();

        QString grps;
        if (hasDialout) {
            grps += "dialout";
        }

        if (hasUucp) {
            if (!grps.isEmpty()) {
                grps += ",";
            }
            grps += "uucp";
        }

        if (hasLock) {
            if (!grps.isEmpty()) {
                grps += ",";
            }
            grps += "lock";
        }

        QProcess process;
        process.setEnvironment(QProcess::systemEnvironment());
        process.start("pkexec", QStringList() << "usermod" << "-aG" << grps << name);
        waitProcess(process);
        if (process.exitCode() == 0) {
            showMessageDialog(tr("Command Result"),
                              tr("Result from command:\n\n"
                                 "%1\n"
                                 "You have to reboot for this "
                                 "change to take effect.").
                              arg(QString(process.readAllStandardOutput())),
                              true, false);
        } else {
            showMessageDialog(tr("Command Result"),
                              tr("Running command failed."),
                              false, false);
        }
        process.close();
    }
#endif
}

void MainWindow::valuesReceived(MC_VALUES values, unsigned int mask)
{
    (void)mask;
    ui->dispCurrent->setVal(values.current_motor);
    ui->dispDuty->setVal(values.duty_now * 100.0);
}

void MainWindow::paramChangedDouble(QObject *src, QString name, double newParam)
{
    (void)src;
    if (name == "l_current_max") {
        ui->dispCurrent->setRange(fabs(newParam));
    }
}

void MainWindow::mcConfigCheckResult(QStringList paramsNotSet)
{
    if (!paramsNotSet.isEmpty()) {
        ParamDialog::showParams(tr("Parameters truncated"),
                                tr("The following parameters were truncated because they were set outside "
                                   "of their allowed limits."),
                                mVesc->mcConfig(),
                                paramsNotSet,
                                this);
    }
}

void MainWindow::on_actionReconnect_triggered()
{
    mVesc->reconnectLastPort();
}

void MainWindow::on_actionDisconnect_triggered()
{
    mVesc->disconnectPort();
}

void MainWindow::on_actionReboot_triggered()
{
    mVesc->commands()->reboot();
}

void MainWindow::on_stopButton_clicked()
{
    mVesc->commands()->setCurrent(0);
    mPageExperiments->stop();
    ui->actionSendAlive->setChecked(false);
}

void MainWindow::on_fullBrakeButton_clicked()
{
    mVesc->commands()->setDutyCycle(0);
    ui->actionSendAlive->setChecked(true);
}

void MainWindow::on_actionReadMcconf_triggered()
{
    mVesc->commands()->getMcconf();
}

void MainWindow::on_actionReadMcconfDefault_triggered()
{
    mVesc->commands()->getMcconfDefault();
}

void MainWindow::on_actionWriteMcconf_triggered()
{
    mVesc->commands()->setMcconf();
}

void MainWindow::on_actionReadAppconf_triggered()
{
    mVesc->commands()->getAppConf();
}

void MainWindow::on_actionReadAppconfDefault_triggered()
{
    mVesc->commands()->getAppConfDefault();
}

void MainWindow::on_actionWriteAppconf_triggered()
{
    mVesc->commands()->setAppConf();
}

void MainWindow::on_actionSaveMotorConfXml_triggered()
{
    QString path;
    path = QFileDialog::getSaveFileName(this,
                                        tr("Choose where to save the motor configuration XML file"),
                                        ".",
                                        tr("Xml files (*.xml)"));

    if (path.isNull()) {
        return;
    }

    if (!path.toLower().endsWith(".xml")) {
        path += ".xml";
    }

    bool res = mVesc->mcConfig()->saveXml(path, "MCConfiguration");

    if (res) {
        showStatusInfo("Saved motor configuration", true);
    } else {
        showMessageDialog(tr("Save motor configuration"),
                          tr("Could not save motor configuration:<BR>"
                             "%1").arg(mVesc->mcConfig()->xmlStatus()),
                          false, false);
    }
}

void MainWindow::on_actionLoadMotorConfXml_triggered()
{
    QString path;
    path = QFileDialog::getOpenFileName(this,
                                        tr("Choose motor configuration file to load"),
                                        ".",
                                        tr("Xml files (*.xml)"));

    if (path.isNull()) {
        return;
    }

    bool res = mVesc->mcConfig()->loadXml(path, "MCConfiguration");

    if (res) {
        showStatusInfo("Loaded motor configuration", true);
    } else {
        showMessageDialog(tr("Load motor configuration"),
                          tr("Could not load motor configuration:<BR>"
                             "%1").arg(mVesc->mcConfig()->xmlStatus()),
                          false, false);
    }
}

void MainWindow::on_actionSaveAppconfXml_triggered()
{
    QString path;
    path = QFileDialog::getSaveFileName(this,
                                        tr("Choose where to save the app configuration XML file"),
                                        ".",
                                        tr("Xml files (*.xml)"));

    if (path.isNull()) {
        return;
    }

    if (!path.toLower().endsWith(".xml")) {
        path += ".xml";
    }

    bool res = mVesc->appConfig()->saveXml(path, "APPConfiguration");

    if (res) {
        showStatusInfo("Saved app configuration", true);
    } else {
        showMessageDialog(tr("Save app configuration"),
                          tr("Could not save app configuration:<BR>"
                             "%1").arg(mVesc->appConfig()->xmlStatus()),
                          false, false);
    }
}

void MainWindow::on_actionLoadAppconfXml_triggered()
{
    QString path;
    path = QFileDialog::getOpenFileName(this,
                                        tr("Choose app configuration file to load"),
                                        ".",
                                        tr("Xml files (*.xml)"));

    if (path.isNull()) {
        return;
    }

    bool res = mVesc->appConfig()->loadXml(path, "APPConfiguration");

    if (res) {
        showStatusInfo("Loaded app configuration", true);
    } else {
        showMessageDialog(tr("Load app configuration"),
                          tr("Could not load app configuration:<BR>"
                             "%1").arg(mVesc->appConfig()->xmlStatus()),
                          false, false);
    }
}

void MainWindow::on_actionExit_triggered()
{
    qApp->exit();
}

void MainWindow::on_actionAbout_triggered()
{
    QMessageBox::about(this, "variESC Tool", Utility::aboutText());
}

void MainWindow::on_actionLibrariesUsed_triggered()
{
    QMessageBox::about(this, "Libraries Used",
                       tr("<b>Icons<br>"
                          "<a href=\"https://icons8.com/\">https://icons8.com/</a><br><br>"
                          "<b>Plotting<br>"
                          "<a href=\"http://qcustomplot.com/\">http://qcustomplot.com/</a>"));
}

void MainWindow::on_dutyButton_clicked()
{
    mVesc->commands()->setDutyCycle(ui->dutyBox->value());
    ui->actionSendAlive->setChecked(true);
}

void MainWindow::on_currentButton_clicked()
{
    mVesc->commands()->setCurrent(ui->currentBox->value());
    ui->actionSendAlive->setChecked(true);
}

void MainWindow::on_speedButton_clicked()
{
    mVesc->commands()->setRpm(int(ui->speedBox->value()));
    ui->actionSendAlive->setChecked(true);
}

void MainWindow::on_posButton_clicked()
{
    mVesc->commands()->setPos(ui->posBox->value());
    ui->actionSendAlive->setChecked(true);
}

void MainWindow::on_brakeCurrentButton_clicked()
{
    mVesc->commands()->setCurrentBrake(ui->brakeCurrentBox->value());
    ui->actionSendAlive->setChecked(true);
}

void MainWindow::on_handbrakeButton_clicked()
{
    mVesc->commands()->setHandbrake(ui->handbrakeBox->value());
    ui->actionSendAlive->setChecked(true);
}

void MainWindow::addPageItem(QString name, QString icon, QString groupIcon, bool bold, bool indented)
{
    QListWidgetItem *item = new QListWidgetItem();
    ui->pageList->addItem(item);
    PageListItem *li = new PageListItem(name, icon, groupIcon, this);
    li->setBold(bold);
    li->setIndented(indented);
    ui->pageList->setItemWidget(item, li);
}

void MainWindow::saveParamFileDialog(QString conf, bool wrapIfdef)
{
    ConfigParams *params = nullptr;

    if (conf.toLower() == "mcconf") {
        params = mVesc->mcConfig();
    } else if (conf.toLower() == "appconf") {
        params = mVesc->appConfig();
    } else {
        qWarning() << "Invalid conf" << conf;
        return;
    }

    QString path;
    path = QFileDialog::getSaveFileName(this,
                                        tr("Choose where to save the configuration header file"),
                                        ".",
                                        tr("h files (*.h)"));

    if (path.isNull()) {
        return;
    }

    if (path.contains(" ")) {
        showMessageDialog(tr("Save header"),
                          tr("Spaces are not allowed in the filename."),
                          false, false);
        return;
    }

    if (!path.toLower().endsWith(".h")) {
        path += ".h";
    }

    bool res = params->saveCDefines(path, wrapIfdef);

    if (res) {
        showStatusInfo("Saved C header", true);
    } else {
        showMessageDialog(tr("Save header"),
                          tr("Could not save header"),
                          false, false);
    }
}

void MainWindow::showPage(const QString &name)
{
    for (int i = 0;i < ui->pageList->count();i++) {
        PageListItem *p = dynamic_cast<PageListItem*>(ui->pageList->itemWidget(ui->pageList->item(i)));
        if (p->name() == name) {
            ui->pageList->setCurrentRow(i);
            break;
        }
    }
}

void MainWindow::reloadPages()
{
    // Remove pages (if any)
    ui->pageList->clear();
    while (ui->pageWidget->count() != 0) {
        QWidget* widget = ui->pageWidget->widget(0);
        ui->pageWidget->removeWidget(widget);
        widget->deleteLater();
    }

    mPageWelcome = new PageWelcome(this);
    mPageWelcome->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageWelcome);
    addPageItem(tr("Welcome & Wizards"), "://res/icons/Home-96.png", "", true);
    connect(ui->actionAutoSetupFOC, SIGNAL(triggered(bool)),
            mPageWelcome, SLOT(startSetupWizardFocSimple()));
    connect(ui->actionMotorSetupWizard, SIGNAL(triggered(bool)),
            mPageWelcome, SLOT(startSetupWizardMotor()));
    connect(ui->actionAppSetupWizard, SIGNAL(triggered(bool)),
            mPageWelcome, SLOT(startSetupWizardApp()));

    mPageConnection = new PageConnection(this);
    mPageConnection->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageConnection);
    addPageItem(tr("Connection"), "://res/icons/Connected-96.png", "", true);

    mPageFirmware = new PageFirmware(this);
    mPageFirmware->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageFirmware);
    addPageItem(tr("Firmware"), "://res/icons/Electronics-96.png", "", true);

    mPageMotorSettings = new PageMotorSettings(this);
    mPageMotorSettings->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageMotorSettings);
    addPageItem(tr("Motor Settings"), "://res/icons/motor.png", "", true);
    mPageNameIdList.insert("motor", ui->pageList->count() - 1);

    mPageMotor = new PageMotor(this);
    mPageMotor->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageMotor);
    addPageItem(tr("General"), "://res/icons/Horizontal Settings Mixer-96.png",
                "://res/icons/mcconf.png", false, true);
    mPageNameIdList.insert("motor_general", ui->pageList->count() - 1);

    mPageBldc = new PageBldc(this);
    mPageBldc->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageBldc);
    addPageItem(tr("BLDC"), "://res/icons/bldc.png",
                "://res/icons/mcconf.png", false, true);
    mPageNameIdList.insert("motor_bldc", ui->pageList->count() - 1);

    mPageDc = new PageDc(this);
    mPageDc->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageDc);
    addPageItem(tr("DC"), "://res/icons/Car Battery-96.png",
                "://res/icons/mcconf.png", false, true);
    mPageNameIdList.insert("motor_dc", ui->pageList->count() - 1);

    mPageFoc = new PageFoc(this);
    mPageFoc->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageFoc);
    addPageItem(tr("FOC"), "://res/icons/3ph_sine.png",
                "://res/icons/mcconf.png", false, true);
    mPageNameIdList.insert("motor_foc", ui->pageList->count() - 1);

    mPageGpd = new PageGPD(this);
    mPageGpd->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageGpd);
    addPageItem(tr("GPDrive"), "://res/icons/3ph_sine.png",
                "://res/icons/mcconf.png", false, true);
    mPageNameIdList.insert("motor_gpdrive", ui->pageList->count() - 1);

    mPageControllers = new PageControllers(this);
    mPageControllers->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageControllers);
    addPageItem(tr("PID Controllers"), "://res/icons/Speed-96.png",
                "://res/icons/mcconf.png", false, true);
    mPageNameIdList.insert("motor_pid", ui->pageList->count() - 1);

    mPageMotorInfo = new PageMotorInfo(this);
    mPageMotorInfo->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageMotorInfo);
    addPageItem(tr("Additional Info"), "://res/icons/About-96.png",
                "://res/icons/mcconf.png", false, true);
    mPageNameIdList.insert("motor_additional_info", ui->pageList->count() - 1);

    mPageExperiments = new PageExperiments(this);
    mPageExperiments->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageExperiments);
    addPageItem(tr("Experiments"), "://res/icons/Calculator-96.png",
                "://res/icons/mcconf.png", false, true);
    mPageNameIdList.insert("motor_experiments", ui->pageList->count() - 1);

    mPageAppSettings = new PageAppSettings(this);
    mPageAppSettings->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageAppSettings);
    addPageItem(tr("App Settings"), "://res/icons/Outgoing Data-96.png", "", true);
    mPageNameIdList.insert("app", ui->pageList->count() - 1);

    mPageAppGeneral = new PageAppGeneral(this);
    mPageAppGeneral->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageAppGeneral);
    addPageItem(tr("General"), "://res/icons/Horizontal Settings Mixer-96.png",
                "://res/icons/appconf.png", false, true);
    mPageNameIdList.insert("app_general", ui->pageList->count() - 1);

    mPageAppPpm = new PageAppPpm(this);
    mPageAppPpm->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageAppPpm);
    addPageItem(tr("PPM"), "://res/icons/Controller-96.png",
                "://res/icons/appconf.png", false, true);
    mPageNameIdList.insert("app_ppm", ui->pageList->count() - 1);

    mPageAppAdc = new PageAppAdc(this);
    mPageAppAdc->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageAppAdc);
    addPageItem(tr("ADC"), "://res/icons/Potentiometer-96.png",
                "://res/icons/appconf.png", false, true);
    mPageNameIdList.insert("app_adc", ui->pageList->count() - 1);

    mPageAppUart = new PageAppUart(this);
    mPageAppUart->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageAppUart);
    addPageItem(tr("UART"), "://res/icons/Rs 232 Male-96.png",
                "://res/icons/appconf.png", false, true);
    mPageNameIdList.insert("app_uart", ui->pageList->count() - 1);

    mPageAppNunchuk = new PageAppNunchuk(this);
    mPageAppNunchuk->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageAppNunchuk);
    addPageItem(tr("variESC Remote"), "://res/icons/icons8-fantasy-96.png",
                "://res/icons/appconf.png", false, true);
    mPageNameIdList.insert("app_vescremote", ui->pageList->count() - 1);

    mPageAppNrf = new PageAppNrf(this);
    mPageAppNrf->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageAppNrf);
    addPageItem(tr("Nrf"), "://res/icons/Online-96.png",
                "://res/icons/appconf.png", false, true);
    mPageNameIdList.insert("app_nrf", ui->pageList->count() - 1);

    mPageAppBalance = new PageAppBalance(this);
    mPageAppBalance->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageAppBalance);
    addPageItem(tr("Balance"), "://res/icons/EUC-96.png",
                "://res/icons/appconf.png", false, true);
    mPageNameIdList.insert("app_balance", ui->pageList->count() - 1);

    mPageAppPas = new PageAppPas(this);
    mPageAppPas->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageAppPas);
    addPageItem(tr("PAS"), "://res/icons/icons8-fantasy-96.png",
                "://res/icons/appconf.png", false, true);
    mPageNameIdList.insert("app_pas", ui->pageList->count() - 1);

    mPageAppImu = new PageAppImu(this);
    mPageAppImu->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageAppImu);
    addPageItem(tr("IMU"), "://res/icons/Gyroscope-96.png",
                "://res/icons/appconf.png", false, true);
    mPageNameIdList.insert("app_imu", ui->pageList->count() - 1);

    mPageCustomConfig0 = new PageCustomConfig(this);
    mPageCustomConfig0->setVesc(mVesc);
    mPageCustomConfig0->setConfNum(0);
    ui->pageWidget->addWidget(mPageCustomConfig0);
    addPageItem(tr("Config0"), "://res/icons/Electronics-96.png", "", true);
    mPageNameIdList.insert("app_custom_config_0", ui->pageList->count() - 1);
    ui->pageList->item(ui->pageList->count() - 1)->setHidden(true);

    mPageCustomConfig1 = new PageCustomConfig(this);
    mPageCustomConfig1->setVesc(mVesc);
    mPageCustomConfig1->setConfNum(1);
    ui->pageWidget->addWidget(mPageCustomConfig1);
    addPageItem(tr("Config1"), "://res/icons/Electronics-96.png", "", true);
    mPageNameIdList.insert("app_custom_config_1", ui->pageList->count() - 1);
    ui->pageList->item(ui->pageList->count() - 1)->setHidden(true);

    mPageCustomConfig2 = new PageCustomConfig(this);
    mPageCustomConfig2->setVesc(mVesc);
    mPageCustomConfig2->setConfNum(2);
    ui->pageWidget->addWidget(mPageCustomConfig2);
    addPageItem(tr("Config0"), "://res/icons/Electronics-96.png", "", true);
    mPageNameIdList.insert("app_custom_config_2", ui->pageList->count() - 1);
    ui->pageList->item(ui->pageList->count() - 1)->setHidden(true);

    mPageDataAnalysis = new PageDataAnalysis(this);
    mPageDataAnalysis->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageDataAnalysis);
    addPageItem(tr("Data Analysis"), "://res/icons/Line Chart-96.png", "", true);

    mPageRtData = new PageRtData(this);
    mPageRtData->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageRtData);
    addPageItem(tr("Realtime Data"), "://res/icons/rt_off.png", "", false, true);
    mPageNameIdList.insert("data_rt", ui->pageList->count() - 1);

    mPageSampledData = new PageSampledData(this);
    mPageSampledData->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageSampledData);
    addPageItem(tr("Sampled Data"), "://res/icons/Gyroscope-96.png", "", false, true);
    mPageNameIdList.insert("data_sampled", ui->pageList->count() - 1);

    mPageImu = new PageImu(this);
    mPageImu->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageImu);
    addPageItem(tr("IMU Data"), "://res/icons/Line Chart-96.png", "", false, true);
    mPageNameIdList.insert("data_imu", ui->pageList->count() - 1);

    mPageBms = new PageBms(this);
    mPageBms->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageBms);
    addPageItem(tr("BMS Data"), "://res/icons/icons8-battery-100.png", "", false, true);
    mPageNameIdList.insert("data_bms", ui->pageList->count() - 1);

    mPageLogAnalysis = new PageLogAnalysis(this);
    mPageLogAnalysis->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageLogAnalysis);
    addPageItem(tr("Log Analysis"), "://res/icons/Waypoint Map-96.png", "", false, true);

    mPageTerminal = new PageTerminal(this);
    mPageTerminal->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageTerminal);
    addPageItem(tr("ESC Terminal"), "://res/icons/Console-96.png", "", true);

    mPageSwdProg = new PageSwdProg(this);
    mPageSwdProg->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageSwdProg);
    addPageItem(tr("SWD Prog"), "://res/icons/Electronics-96.png", "", true);

    mPageCanAnalyzer = new PageCanAnalyzer(this);
    mPageCanAnalyzer->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageCanAnalyzer);
    addPageItem(tr("CAN Analyzer"), "://res/icons/can_off.png", "", true);

    mPageDebugPrint = new PageDebugPrint(this);
    ui->pageWidget->addWidget(mPageDebugPrint);
    addPageItem(tr("Debug Console"), "://res/icons/Bug-96.png", "", true);

    mPageSettings = new PageSettings(this);
    mPageSettings->setVesc(mVesc);
    ui->pageWidget->addWidget(mPageSettings);
    addPageItem(tr("Settings"), "://res/icons/Settings-96.png", "", true);

    /*
     * Page IDs
     *
     * motor
     * motor_general
     * motor_bldc
     * motor_dc
     * motor_foc
     * motor_gpdrive
     * motor_pid
     * motor_additional_info
     * motor_experiments
     * app
     * app_general
     * app_ppm
     * app_adc
     * app_uart
     * app_vescremote
     * app_nrf
     * app_balance
     * app_imu
     * app_custom_config_0
     * app_custom_config_1
     * app_custom_config_2
     * data_rt
     * data_sampled
     * data_imu
     * data_bms
     */

    // Adjust sizes
    QFontMetrics fm(this->font());
    int width = fm.width("Welcome & Wizards++++++++++");
    int height = fm.height();

    for(int i = 0; i < ui->pageList->count(); i++) {
        QListWidgetItem *item = ui->pageList->item(i);
        item->setSizeHint(QSize(item->sizeHint().width(), height));
    }

    ui->pageList->setMinimumWidth(width);
    ui->pageList->setMaximumWidth(width);
    ui->canList->setMinimumWidth(width);
    ui->canList->setMaximumWidth(width);
    ui->pageLabel->setMaximumWidth(width);
    ui->pageLabel->setMaximumHeight((394 * width) / 1549);
    ui->scanCanButton->setMaximumWidth(width);

    ui->pageList->setCurrentRow(0);
    ui->pageWidget->setCurrentIndex(0);
}

void MainWindow::checkUdev()
{
    // Check if udev rules for modemmanager are installed
#ifdef Q_OS_LINUX
    QFileInfo fi_mm("/lib/udev/rules.d/77-mm-usb-device-blacklist.rules");
    if (fi_mm.exists()) {
        QFileInfo fi_vesc("/lib/udev/rules.d/45-vesc.rules");
        if (!fi_vesc.exists()) {
            QMessageBox::StandardButton reply;
            reply = QMessageBox::information(this,
                                             tr("Modemmenager"),
                                             tr("It looks like modemmanager is installed on your system, and that "
                                                "there are no variESC udev rules installed. This will cause a delay "
                                                "from when you plug in the ESC until you can use it. Would you like "
                                                "to add a udev rule to prevent modemmanager from grabbing the ESC?"),
                                             QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

            if (reply == QMessageBox::Yes) {
                QFile f_vesc(QDir::temp().absoluteFilePath(fi_vesc.fileName()));
                if (!f_vesc.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    showMessageDialog(tr("Create File Error"),
                                      f_vesc.errorString(),
                                      false, false);
                    return;
                }

                f_vesc.write("# Prevent modemmanager from grabbing the ESC\n"
                             "ATTRS{idVendor}==\"0483\", ATTRS{idProduct}==\"5740\", ENV{ID_MM_DEVICE_IGNORE}=\"1\"\n");
                f_vesc.close();

                QFileInfo fi_new(f_vesc);
                QProcess process;
                process.setEnvironment(QProcess::systemEnvironment());
                process.start("pkexec", QStringList() <<
                              "mv" <<
                              fi_new.absoluteFilePath() <<
                              fi_vesc.absolutePath());
                waitProcess(process);

                if (process.exitCode() == 0) {
                    QMessageBox::StandardButton reply;
                    reply = QMessageBox::information(this,
                                                     tr("Modemmenager"),
                                                     tr("The udev rule was created successfully. Would you like "
                                                        "to reload udev to apply the new rule?"),
                                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

                    if (reply == QMessageBox::Yes) {
                        QProcess process;
                        process.setEnvironment(QProcess::systemEnvironment());
                        process.start("pkexec", QStringList() <<
                                      "udevadm" <<
                                      "control" <<
                                      "--reload-rules");
                        waitProcess(process);

                        if (process.exitCode() == 0) {
                            showMessageDialog(tr("Command Result"),
                                              tr("Reloaded udev rules sucessfully."),
                                              true, false);
                        } else {
                            QString out = process.readAll();

                            if (out.isEmpty()) {
                                showMessageDialog(tr("Command Result"),
                                                  tr("Could not reload udev rules. A reboot is probably "
                                                     "required for this change to take effect."),
                                                  false, false);
                            }
                        }
                        process.close();
                    }
                } else {
                    showMessageDialog(tr("Command Result"),
                                      tr("Could not move rules file:\n\n"
                                         "%1").
                                      arg(QString(process.readAllStandardOutput())),
                                      false, false);
                }
                process.close();
            }
        }
    }
#endif
}

bool MainWindow::waitProcess(QProcess &process, bool block, int timeoutMs)
{
    bool wasEnables = isEnabled();
    bool killed = false;

    if (block) {
        setEnabled(false);
    }

    process.waitForStarted();

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.start(timeoutMs);
    connect(&process, SIGNAL(finished(int)), &loop, SLOT(quit()));
    connect(&timeoutTimer, SIGNAL(timeout()), &loop, SLOT(quit()));
    loop.exec();

    if (process.state() == QProcess::Running) {
        process.kill();
        process.waitForFinished();
        killed = true;
    }

    setEnabled(wasEnables);

    return !killed;
}

QString MainWindow::runCmd(QString cmd, QStringList args)
{
    QProcess process;
    process.setEnvironment(QProcess::systemEnvironment());
    process.start(cmd, args);
    waitProcess(process);
    QString res = process.readAllStandardOutput();
    process.close();
    return res;
}

void MainWindow::on_pageList_currentRowChanged(int currentRow)
{
    if (currentRow >= 0) {
        ui->pageWidget->setCurrentIndex(currentRow);
    }
}

void MainWindow::on_actionParameterEditorMcconf_triggered()
{
    ParameterEditor *p = new ParameterEditor(this);
    p->setAttribute(Qt::WA_DeleteOnClose);
    p->setParams(mVesc->mcConfig());
    p->show();
}

void MainWindow::on_actionParameterEditorAppconf_triggered()
{
    ParameterEditor *p = new ParameterEditor(this);
    p->setAttribute(Qt::WA_DeleteOnClose);
    p->setParams(mVesc->appConfig());
    p->show();
}

void MainWindow::on_actionParameterEditorInfo_triggered()
{
    ParameterEditor *p = new ParameterEditor(this);
    p->setAttribute(Qt::WA_DeleteOnClose);
    p->setParams(mVesc->infoConfig());
    p->show();
}

void MainWindow::on_actionParameterEditorFW_triggered()
{
    ParameterEditor *p = new ParameterEditor(this);
    p->setAttribute(Qt::WA_DeleteOnClose);
    p->setParams(mVesc->fwConfig());
    p->show();
}

void MainWindow::on_actionParameterEditorCustomConf0_triggered()
{
    auto conf = mVesc->customConfig(0);

    if (conf) {
        ParameterEditor *p = new ParameterEditor(this);
        p->setAttribute(Qt::WA_DeleteOnClose);
        p->setParams(conf);
        p->show();
    } else {
        mVesc->emitMessageDialog("Edit Custom Config 0",
                                 "The connected hardware does not have a custom configuration.",
                                 false);
    }
}

void MainWindow::on_actionSaveMotorConfigurationHeader_triggered()
{
    saveParamFileDialog("mcconf", false);
}

void MainWindow::on_actionSaveAppConfigurationHeader_triggered()
{
    saveParamFileDialog("appconf", false);
}

void MainWindow::on_actionSaveMotorConfigurationHeaderWrap_triggered()
{
    saveParamFileDialog("mcconf", true);
}

void MainWindow::on_actionSaveAppConfigurationHeaderWrap_triggered()
{
    saveParamFileDialog("appconf", true);
}

void MainWindow::on_actionTerminalPrintFaults_triggered()
{
    mVesc->commands()->sendTerminalCmd("faults");
    showPage("ESC Terminal");
}

void MainWindow::on_actionTerminalShowHelp_triggered()
{
    mVesc->commands()->sendTerminalCmd("help");
    showPage("ESC Terminal");
}

void MainWindow::on_actionTerminalClear_triggered()
{
    mPageTerminal->clearTerminal();
    showPage("ESC Terminal");
}

void MainWindow::on_actionTerminalPrintThreads_triggered()
{
    mVesc->commands()->sendTerminalCmd("threads");
    showPage("ESC Terminal");
}

void MainWindow::on_actionTerminalDRVResetLatchedFaults_triggered()
{
    mVesc->commands()->sendTerminalCmd("drv8301_reset_faults");
    mVesc->commands()->sendTerminalCmd("drv8323s_reset_faults");
    mVesc->commands()->sendTerminalCmd("drv8320_reset_faults");
    mVesc->commands()->sendTerminalCmd("drv8305_reset_faults");
}

void MainWindow::on_actionCanFwd_toggled(bool arg1)
{
    if (arg1 && mVesc->commands()->getCanSendId() < 0) {
        ui->actionCanFwd->setChecked(false);
        mVesc->emitMessageDialog("CAN Forward",
                                 "No CAN device is selected. Go to the connection page and select one.",
                                 false, false);
    } else {
        mVesc->commands()->setSendCan(arg1);
    }
}

void MainWindow::on_actionSafetyInformation_triggered()
{
    HelpDialog::showHelp(this, mVesc->infoConfig(), "wizard_startup_usage");
}

void MainWindow::on_actionWarrantyStatement_triggered()
{
    HelpDialog::showHelp(this, mVesc->infoConfig(), "wizard_startup_warranty");
}

void MainWindow::on_actionVESCToolChangelog_triggered()
{
    HelpDialog::showHelp(this, "variESC Tool Changelog", Utility::vescToolChangeLog());
}

void MainWindow::on_actionFirmwareChangelog_triggered()
{
    HelpDialog::showHelp(this, "Firmware Changelog", Utility::fwChangeLog());
}

void MainWindow::on_actionVESCProjectForums_triggered()
{
    QDesktopServices::openUrl(QUrl("https://forum.esk8.news/t/variesc-tool-sro/46097"));
}

void MainWindow::on_actionLicense_triggered()
{
    HelpDialog::showHelp(this, mVesc->infoConfig(), "gpl_text");
}

void MainWindow::on_posBox_editingFinished()
{
    on_posButton_clicked();
}

void MainWindow::on_posBox_valueChanged(double arg1)
{
    (void)arg1;
//    on_posButton_clicked();
}

void MainWindow::on_actionExportConfigurationParser_triggered()
{
    QString path;
    path = QFileDialog::getSaveFileName(this,
                                        tr("Choose where to save the parser C source and header file"),
                                        ".",
                                        tr("C Source/Header files (*.c *.h)"));

    if (path.isNull()) {
        return;
    }

    Utility::createParamParserC(mVesc, path);
}

void MainWindow::on_actionBackupConfiguration_triggered()
{
    bool ok;
    QString name = QInputDialog::getText(this, "Backup name (Optional)",
                                         "Name (can be blank):", QLineEdit::Normal,
                                         "", &ok);
    if (ok) {
        mVesc->confStoreBackup(false, name);
    }
}

void MainWindow::on_actionRestoreConfiguration_triggered()
{
    mVesc->confRestoreBackup(false);
}

void MainWindow::on_actionClearConfigurationBackups_triggered()
{
    QMessageBox::StandardButton reply;
    reply = QMessageBox::warning(this,
                                 tr("Warning"),
                                 tr("This is going to remove all configuration backups for "
                                    "this instance of variESC Tool. Continue?"),
                                 QMessageBox::Yes | QMessageBox::Cancel);

    if (reply == QMessageBox::Yes) {
        mVesc->confClearBackups();
    }
}

void MainWindow::on_actionBackupConfigurationsCAN_triggered()
{
    bool ok;
    QString name = QInputDialog::getText(this, "Backup name (Optional)",
                                         "Name (can be blank):", QLineEdit::Normal,
                                         "", &ok);
    if (ok) {
        QProgressDialog dialog("Backing up configurations...", QString(), 0, 0, this);
        dialog.setWindowModality(Qt::WindowModal);
        dialog.show();
        mVesc->confStoreBackup(true, name);
    }
}

void MainWindow::on_actionRestoreConfigurationsCAN_triggered()
{
    QProgressDialog dialog("Restoring configurations...", QString(), 0, 0, this);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.show();
    mVesc->confRestoreBackup(true);
}

void MainWindow::on_scanCanButton_clicked()
{
    if (mVesc->isPortConnected()) {
        ui->scanCanButton->setEnabled(false);
        mVesc->commands()->setSendCan(false);
        mVesc->commands()->pingCan();
        ui->canList->clear();
    } else {
        ui->canList->clear();
        showStatusInfo("Connect to ESC before scanning", false);
    }
}

void MainWindow::pingCanRx(QVector<int> devs, bool isTimeout)
{
    if (isTimeout || ui->canList->count() > 0) {
        return;
    }

    ui->scanCanButton->setEnabled(false);

    ui->canList->clear();
    FW_RX_PARAMS params;

    auto typeToStr = [](FW_RX_PARAMS p, QString idStr, bool ok) {
        QString res;

        if (ok) {
            if (p.hwType == HW_TYPE_VESC) {
                res = "Motor (" + p.hw + "): " + idStr;
            } else if (p.hwType == HW_TYPE_VESC_BMS) {
                res = "BMS (" + p.hw + "): " + idStr;
            } else {
                res = "Device (" + p.hw + "): " + idStr;
            }
        } else {
            res = "Unknown: " + idStr;
        }

        return res;
    };

    bool ok = false;
    QListWidgetItem *item = nullptr;
    for (int dev: devs) {
        ok = Utility::getFwVersionBlockingCan(mVesc, &params, dev);
        item = new QListWidgetItem(typeToStr(params, QString::number(dev), ok));
        ui->canList->addItem(item);
    }

    // Read local firmware version last so that firmware pages show the correct info.
    ok = Utility::getFwVersionBlocking(mVesc, &params);
    item = new QListWidgetItem(typeToStr(params, "Local", ok));
    ui->canList->insertItem(0, item);
    ui->canList->setCurrentRow(0);

    ui->scanCanButton->setEnabled(true);
}

void MainWindow::on_canList_currentRowChanged(int currentRow)
{
    if (currentRow >= 0) {
        if (currentRow == 0) {
            if (mVesc->commands()->getSendCan()) {
                mVesc->commands()->setSendCan(false);
                QTimer::singleShot(1500, [this]() {
                    if (mVesc->fwRx() && mVesc->getLastFwRxParams().hwType == HW_TYPE_VESC) {
                        mVesc->commands()->getMcconf();
                        mVesc->commands()->getAppConf();
                    }
                });
            }
        } else {
            int id = ui->canList->currentItem()->text().split(" ").last().toInt();
            if (id >= 0 && id < 255) {
                if (!mVesc->commands()->getSendCan() || mVesc->commands()->getCanSendId() != id) {
                    mVesc->commands()->setCanSendId(quint32(id));
                    mVesc->commands()->setSendCan(true);
                    QTimer::singleShot(1500, [this]() {
                        if (mVesc->fwRx() && mVesc->getLastFwRxParams().hwType == HW_TYPE_VESC) {
                            mVesc->commands()->getMcconf();
                            mVesc->commands()->getAppConf();
                        }
                    });
                }
            }
        }
    }
}

void MainWindow::on_actionGamepadControl_triggered(bool checked)
{
    mPageSettings->setUseGamepadControl(checked);

    if (!mPageSettings->isUsingGamepadControl()) {
        ui->actionGamepadControl->setChecked(false);
    }
}

void MainWindow::on_actionLoadMeters_triggered()
{
#ifdef USE_MOBILE
    mQmlUi.startCustomGui(mVesc);
    mQmlUi.reloadCustomGui("qrc:/res/qml/Meters.qml");
#endif
}

void MainWindow::on_actionCloseCustomGUI_triggered()
{
#ifdef USE_MOBILE
    mQmlUi.stopCustomGui();
#endif
}
