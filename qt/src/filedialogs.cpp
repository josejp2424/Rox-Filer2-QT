#include "filedialogs.h"
#include "fileops.h"
#include "mimeapps.h"
#include "xattrs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QTableWidget>
#include <QFile>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

QLabel *selectableLabel(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setTextFormat(Qt::PlainText); // arbitrary filenames must not become HTML
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setWordWrap(true);
    return l;
}

QString formatTime(const QDateTime &dt)
{
    return dt.isValid() ? QLocale().toString(dt, QLocale::LongFormat) : QStringLiteral("—");
}

QString userName()
{
    const QString env = qEnvironmentVariable("USER");
    if (!env.isEmpty())
        return env;
    if (const passwd *pw = ::getpwuid(::getuid()))
        return QString::fromLocal8Bit(pw->pw_name);
    return QStringLiteral("root");
}

} // namespace

/* ------------------------------------------------------------- Open With */

void RoxDialogs::chooseApplication(QWidget *parent, const QStringList &paths, bool setDefaultOnly)
{
    if (paths.isEmpty())
        return;
    const QString mime = MimeApps::commonMimeType(paths);
    if (mime.isEmpty()) {
        QMessageBox::warning(parent, tr("Open With"), tr("The selected items do not share one MIME type."));
        return;
    }
    static const QMimeDatabase mdb;
    const QMimeType mt = mdb.mimeTypeForName(mime);
    const QString comment = mt.comment().isEmpty() ? mime : mt.comment();

    QDialog dlg(parent);
    dlg.setWindowTitle(setDefaultOnly ? tr("Set Default Application") : tr("Open With"));
    dlg.resize(580, 500);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(tr("Applications for %1 (%2)").arg(comment.toHtmlEscaped(), mime), &dlg));

    auto *search = new QLineEdit(&dlg);
    search->setPlaceholderText(tr("Search applications…"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);

    auto *list = new QListWidget(&dlg);
    list->setIconSize(QSize(24, 24));
    layout->addWidget(list, 1);

    QList<DesktopApp> entries;
    const QList<DesktopApp> recommended = MimeApps::appsForMime(mime);
    const DesktopApp def = MimeApps::defaultAppForMime(mime);
    QStringList ids;
    for (const DesktopApp &a : recommended) {
        auto *item = new QListWidgetItem(a.qicon(), a.id == def.id
            ? tr("%1 (default)").arg(a.name) : a.name, list);
        item->setData(Qt::UserRole, int(entries.size()));
        item->setToolTip(a.exec);
        entries << a;
        ids << a.id;
    }
    QListWidgetItem *otherHeader = nullptr;
    const QList<DesktopApp> others = MimeApps::allApps();
    for (const DesktopApp &a : others) {
        if (ids.contains(a.id))
            continue;
        if (!otherHeader && !recommended.isEmpty()) {
            otherHeader = new QListWidgetItem(tr("Other applications"), list);
            otherHeader->setFlags(Qt::NoItemFlags);
            QFont f = otherHeader->font();
            f.setBold(true);
            otherHeader->setFont(f);
        }
        auto *item = new QListWidgetItem(a.qicon(), a.name, list);
        item->setData(Qt::UserRole, int(entries.size()));
        item->setToolTip(a.exec);
        entries << a;
    }
    if (list->count() > 0)
        list->setCurrentRow(0);

    auto *custom = new QLineEdit(&dlg);
    custom->setPlaceholderText(tr("Or enter a command, e.g. gimp %F"));
    layout->addWidget(custom);

    auto *remember = new QCheckBox(tr("Remember as default application for “%1”").arg(comment), &dlg);
    remember->setChecked(setDefaultOnly);
    remember->setEnabled(!setDefaultOnly);
    layout->addWidget(remember);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(setDefaultOnly ? tr("Set") : tr("Open"));
    layout->addWidget(buttons);

    QObject::connect(search, &QLineEdit::textChanged, list, [list](const QString &text) {
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem *it = list->item(i);
            if (!(it->flags() & Qt::ItemIsEnabled)) {
                it->setHidden(!text.isEmpty());
                continue;
            }
            it->setHidden(!text.isEmpty() && !it->text().contains(text, Qt::CaseInsensitive));
        }
    });
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return;

    QString error;
    DesktopApp app;
    if (!custom->text().trimmed().isEmpty()) {
        app = MimeApps::createCustomApp(custom->text(), mime, &error);
    } else if (QListWidgetItem *it = list->currentItem()) {
        const QVariant v = it->data(Qt::UserRole);
        if (v.isValid())
            app = entries.value(v.toInt());
    }
    if (!app.isValid()) {
        if (!error.isEmpty())
            QMessageBox::warning(parent, dlg.windowTitle(), error);
        return;
    }
    if (remember->isChecked() && !MimeApps::setDefaultApp(mime, app.id, &error))
        QMessageBox::warning(parent, dlg.windowTitle(), tr("Could not save the default application: %1").arg(error));
    if (!setDefaultOnly &&
        !MimeApps::launch(app, paths, QFileInfo(paths.first()).absolutePath(), &error))
        QMessageBox::warning(parent, dlg.windowTitle(), error);
}

/* ------------------------------------------------------------ Properties */

void RoxDialogs::showProperties(QWidget *parent, const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    auto *dlg = new QDialog(parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(dlg);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    static const QMimeDatabase mdb;
    bool anyDir = false;
    QLabel *permissionsValue = nullptr;

    if (paths.size() == 1) {
        const QString path = paths.first();
        const QFileInfo fi(path);
        dlg->setWindowTitle(tr("Properties of “%1”").arg(fi.fileName().isEmpty() ? path : fi.fileName()));
        auto *heading = new QHBoxLayout;
        auto *image = new QLabel(dlg);
        image->setPixmap(QFileIconProvider().icon(fi).pixmap(48, 48));
        heading->addWidget(image, 0, Qt::AlignTop);
        auto *headingText = selectableLabel(fi.fileName().isEmpty() ? path : fi.fileName(), dlg);
        QFont headingFont = headingText->font();
        headingFont.setBold(true);
        headingText->setFont(headingFont);
        heading->addWidget(headingText, 1);
        layout->insertLayout(0, heading);
        const QMimeType mt = fi.isDir() ? mdb.mimeTypeForName(QStringLiteral("inode/directory"))
                                        : mdb.mimeTypeForFile(fi);
        form->addRow(tr("Name:"), selectableLabel(fi.fileName().isEmpty() ? path : fi.fileName(), dlg));
        form->addRow(tr("Location:"), selectableLabel(fi.absolutePath(), dlg));
        form->addRow(tr("Type:"), selectableLabel(QStringLiteral("%1 (%2)").arg(mt.comment(), mt.name()), dlg));
        if (fi.isSymLink())
            form->addRow(tr("Link target:"), selectableLabel(fi.symLinkTarget(), dlg));
        if (fi.isDir()) {
            anyDir = true;
            form->addRow(tr("Size:"), selectableLabel(tr("Use Count to measure the contents"), dlg));
        } else {
            form->addRow(tr("Size:"), selectableLabel(QStringLiteral("%1 (%2 bytes)")
                         .arg(FileOps::humanSize(fi.size()), QLocale().toString(fi.size())), dlg));
        }
        form->addRow(tr("Owner:"), selectableLabel(QStringLiteral("%1 (%2)").arg(fi.owner()).arg(fi.ownerId()), dlg));
        form->addRow(tr("Group:"), selectableLabel(QStringLiteral("%1 (%2)").arg(fi.group()).arg(fi.groupId()), dlg));
        permissionsValue = selectableLabel(FileOps::permissionString(path), dlg);
        permissionsValue->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        form->addRow(tr("Permissions:"), permissionsValue);
        form->addRow(tr("Created:"), selectableLabel(formatTime(fi.birthTime()), dlg));
        form->addRow(tr("Modified:"), selectableLabel(formatTime(fi.lastModified()), dlg));
        form->addRow(tr("Accessed:"), selectableLabel(formatTime(fi.lastRead()), dlg));
        form->addRow(tr("Changed:"), selectableLabel(formatTime(fi.metadataChangeTime()), dlg));
        const QStorageInfo st(path);
        if (st.isValid()) {
            form->addRow(tr("Filesystem:"), selectableLabel(QStringLiteral("%1 — %2 (%3)")
                .arg(st.rootPath(), QString::fromLocal8Bit(st.device()),
                     QString::fromLatin1(st.fileSystemType())), dlg));
            form->addRow(tr("Free space:"), selectableLabel(FileOps::humanSize(st.bytesAvailable()), dlg));
        }
    } else {
        dlg->setWindowTitle(tr("Properties of %1 items").arg(paths.size()));
        int files = 0, dirs = 0;
        qint64 total = 0;
        QString common = QFileInfo(paths.first()).absolutePath();
        for (const QString &p : paths) {
            const QFileInfo fi(p);
            if (fi.isDir()) { ++dirs; anyDir = true; }
            else { ++files; total += fi.size(); }
            if (fi.absolutePath() != common) common.clear();
        }
        form->addRow(tr("Items:"), selectableLabel(tr("%1 files, %2 folders").arg(files).arg(dirs), dlg));
        form->addRow(tr("Size of files:"), selectableLabel(FileOps::humanSize(total), dlg));
        if (!common.isEmpty())
            form->addRow(tr("Location:"), selectableLabel(common, dlg));
    }

    // GTK Rox-Filer2 infobox.c: permissions are editable directly in the
    // Properties window. Qt's old read-only mode string omitted this feature.
    // Unlike GTK's immediate-on-toggle chmod, Qt uses explicit Apply to avoid
    // accidentally changing permissions while inspecting files as root.
    if (paths.size() == 1) {
        const QString path = paths.first();
        struct stat st{};
        const QByteArray native = QFile::encodeName(path);
        // stat(), like chmod(), operates on the symlink target.
        if (::stat(native.constData(), &st) == 0) {
            auto *group = new QGroupBox(tr("Edit permissions"), dlg);
            auto *matrix = new QGridLayout(group);
            matrix->addWidget(new QLabel(tr("Read"), group), 0, 1);
            matrix->addWidget(new QLabel(tr("Write"), group), 0, 2);
            matrix->addWidget(new QLabel(tr("Execute"), group), 0, 3);
            matrix->addWidget(new QLabel(tr("Special"), group), 0, 4);
            const std::array<mode_t, 12> masks{
                S_IRUSR, S_IWUSR, S_IXUSR, S_ISUID,
                S_IRGRP, S_IWGRP, S_IXGRP, S_ISGID,
                S_IROTH, S_IWOTH, S_IXOTH, S_ISVTX
            };
            std::array<QCheckBox *, 12> boxes{};
            const QStringList ownerLabels{tr("Owner"), tr("Group"), tr("Others")};
            const QStringList specialLabels{tr("SUID"), tr("SGID"), tr("Sticky")};
            for (int row = 0; row < 3; ++row) {
                matrix->addWidget(new QLabel(ownerLabels.at(row), group), row + 1, 0);
                for (int col = 0; col < 4; ++col) {
                    const int bit = row * 4 + col;
                    auto *check = new QCheckBox(col == 3 ? specialLabels.at(row) : QString(), group);
                    check->setChecked((st.st_mode & masks.at(bit)) != 0);
                    boxes.at(bit) = check;
                    matrix->addWidget(check, row + 1, col + 1);
                }
            }
            auto *apply = new QPushButton(tr("Apply permissions"), group);
            auto *outcome = new QLabel(group);
            outcome->setTextInteractionFlags(Qt::TextSelectableByMouse);
            auto *line = new QHBoxLayout;
            line->addWidget(apply);
            line->addWidget(outcome, 1);
            matrix->addLayout(line, 4, 0, 1, 5);
            if (QFileInfo(path).isSymLink()) {
                auto *note = new QLabel(tr("For a symbolic link, changes apply to its target."), group);
                note->setWordWrap(true);
                matrix->addWidget(note, 5, 0, 1, 5);
            }
            QObject::connect(apply, &QPushButton::clicked, dlg,
                             [dlg, path, masks, boxes, outcome, permissionsValue]() {
                mode_t newMode = 0;
                for (int i = 0; i < 12; ++i)
                    if (boxes.at(i)->isChecked()) newMode |= masks.at(i);
                const QByteArray encoded = QFile::encodeName(path);
                if (::chmod(encoded.constData(), newMode) != 0) {
                    const QString reason = QString::fromLocal8Bit(::strerror(errno));
                    QMessageBox::warning(dlg, tr("Permissions"),
                        tr("Could not change permissions of %1:\n%2").arg(path, reason));
                    outcome->clear();
                } else {
                    outcome->setText(tr("Permissions applied."));
                    permissionsValue->setText(FileOps::permissionString(path));
                }
            });
            layout->addWidget(group);
        }
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    // The separate Permissions dialog remains available for multi-selection
    // and recursive chmod, even though single-file properties have a grid.
    auto *permissionsButton = buttons->addButton(tr("Permissions…"), QDialogButtonBox::ActionRole);
    QObject::connect(permissionsButton, &QPushButton::clicked, dlg,
                     [dlg, paths] { changePermissions(dlg, paths, nullptr); });
    if (anyDir) {
        QPushButton *count = buttons->addButton(tr("Count"), QDialogButtonBox::ActionRole);
        QObject::connect(count, &QPushButton::clicked, dlg, [dlg, paths] { showCount(dlg, paths); });
    }
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    layout->addWidget(buttons);
    dlg->resize(650, dlg->sizeHint().height());
    dlg->show();
}

/* ----------------------------------------------------------------- Count */

void RoxDialogs::showCount(QWidget *parent, const QStringList &paths)
{
    struct State {
        std::atomic<qint64> bytes{0};
        std::atomic<qint64> files{0};
        std::atomic<qint64> dirs{0};
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
    };
    auto state = std::make_shared<State>();

    auto *dlg = new QDialog(parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Count"));
    auto *layout = new QVBoxLayout(dlg);
    auto *label = new QLabel(dlg);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setMinimumWidth(360);
    layout->addWidget(label);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    QObject::connect(dlg, &QObject::destroyed, [state] { state->cancel = true; });

    QThread *worker = QThread::create([state, paths] {
        auto account = [&state](const QString &p) {
            struct stat st;
            if (::lstat(QFile::encodeName(p).constData(), &st) != 0)
                return false;
            if (S_ISDIR(st.st_mode)) ++state->dirs;
            else ++state->files;
            state->bytes += st.st_size;
            return S_ISDIR(st.st_mode);
        };
        for (const QString &p : paths) {
            if (state->cancel) break;
            if (!account(p))
                continue;
            QDirIterator it(p, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
            while (it.hasNext() && !state->cancel)
                account(it.next());
        }
        state->done = true;
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);

    auto *timer = new QTimer(dlg);
    auto update = [state, label, timer, paths] {
        const QString text = tr("%1 files, %2 folders\nTotal: %3 (%4 bytes)")
            .arg(QLocale().toString(qlonglong(state->files)))
            .arg(QLocale().toString(qlonglong(state->dirs)))
            .arg(FileOps::humanSize(state->bytes))
            .arg(QLocale().toString(qlonglong(state->bytes)));
        label->setText((state->done ? QString() : tr("Counting…") + QLatin1Char('\n')) + text);
        if (state->done)
            timer->stop();
    };
    QObject::connect(timer, &QTimer::timeout, dlg, update);
    timer->start(150);
    update();
    worker->start();
    dlg->show();
}

/* ----------------------------------------------------------- Permissions */

void RoxDialogs::changePermissions(QWidget *parent, const QStringList &paths, std::function<void()> done)
{
    if (paths.isEmpty())
        return;
    bool anyDir = false;
    for (const QString &p : paths)
        if (QFileInfo(p).isDir() && !QFileInfo(p).isSymLink()) anyDir = true;

    QDialog dlg(parent);
    dlg.setWindowTitle(tr("Permissions"));
    auto *layout = new QVBoxLayout(&dlg);
    if (paths.size() == 1) {
        auto *current = new QLabel(QStringLiteral("%1\n%2").arg(QFileInfo(paths.first()).fileName(),
                                   FileOps::permissionString(paths.first())), &dlg);
        current->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        layout->addWidget(current);
    } else {
        layout->addWidget(new QLabel(tr("%1 items selected").arg(paths.size()), &dlg));
    }
    layout->addWidget(new QLabel(tr("Enter a chmod-style command, e.g. a+x, go-w, u=rw,go=r or 0644:"), &dlg));
    auto *mode = new QComboBox(&dlg);
    mode->setEditable(true);
    mode->addItems({QStringLiteral("a+r"), QStringLiteral("a+x"), QStringLiteral("a-x"),
                    QStringLiteral("u+w"), QStringLiteral("go-w"), QStringLiteral("u=rw,go=r"),
                    QStringLiteral("u=rwx,go=rx"), QStringLiteral("0644"), QStringLiteral("0755"),
                    QStringLiteral("0600"), QStringLiteral("0700")});
    const QString last = QSettings().value(QStringLiteral("permissions/last")).toString();
    mode->setCurrentText(last.isEmpty() ? QStringLiteral("a+r") : last);
    const QRegularExpression valid(QStringLiteral(
        "^([0-7]{3,4}|[ugoa]*[-+=][rwxXst]*(,[ugoa]*[-+=][rwxXst]*)*)$"));
    mode->setValidator(new QRegularExpressionValidator(valid, mode));
    layout->addWidget(mode);
    auto *recursive = new QCheckBox(tr("Apply to the contents of folders too"), &dlg);
    recursive->setEnabled(anyDir);
    layout->addWidget(recursive);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString m = mode->currentText().trimmed();
    if (!valid.match(m).hasMatch()) {
        QMessageBox::warning(parent, tr("Permissions"), tr("“%1” is not a valid chmod command.").arg(m));
        return;
    }
    QSettings().setValue(QStringLiteral("permissions/last"), m);
    QStringList cmd{QStringLiteral("chmod")};
    if (recursive->isChecked())
        cmd << QStringLiteral("-R");
    cmd << m << QStringLiteral("--") << paths;
    QPointer<QWidget> guard(parent);
    FileJob::run(parent, tr("Permissions"), {cmd}, [guard, done](bool, const QString &errors) {
        if (!errors.isEmpty())
            QMessageBox::warning(guard, tr("Permissions"), errors);
        if (done) done();
    });
}

/* ---------------------------------------------------------- File actions */

void RoxDialogs::addFileAction(QWidget *parent, const QStringList &paths)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(tr("Add File Action"));
    auto *layout = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout;
    layout->addLayout(form);

    const QString mime = MimeApps::commonMimeType(paths);
    bool hasFiles = false, hasDirs = false, hasLinks = false;
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        if (fi.isSymLink()) hasLinks = true;
        else if (fi.isDir()) hasDirs = true;
        else hasFiles = true;
    }

    auto *name = new QLineEdit(&dlg);
    auto *command = new QLineEdit(&dlg);
    command->setPlaceholderText(tr("e.g. gimp %F   (%f = one file, %F = all selected files)"));
    auto *icon = new QLineEdit(QStringLiteral("system-run"), &dlg);
    auto *mimes = new QLineEdit(mime, &dlg);
    mimes->setPlaceholderText(tr("e.g. image/*; empty = all types"));
    auto *terminal = new QCheckBox(tr("Run in terminal"), &dlg);
    auto *tFiles = new QCheckBox(tr("Files"), &dlg);
    auto *tDirs = new QCheckBox(tr("Folders"), &dlg);
    auto *tLinks = new QCheckBox(tr("Links"), &dlg);
    tFiles->setChecked(hasFiles || paths.isEmpty());
    tDirs->setChecked(hasDirs);
    tLinks->setChecked(hasLinks);
    auto *targets = new QHBoxLayout;
    targets->addWidget(tFiles);
    targets->addWidget(tDirs);
    targets->addWidget(tLinks);
    targets->addStretch(1);
    auto *selection = new QComboBox(&dlg);
    selection->addItem(tr("Any number of items"), QStringLiteral("any"));
    selection->addItem(tr("Only one item"), QStringLiteral("single"));
    selection->addItem(tr("Two or more items"), QStringLiteral("multiple"));

    form->addRow(tr("Name:"), name);
    form->addRow(tr("Command:"), command);
    form->addRow(tr("Icon:"), icon);
    form->addRow(tr("MIME types:"), mimes);
    form->addRow(tr("Applies to:"), targets);
    form->addRow(tr("Selection:"), selection);
    form->addRow(QString(), terminal);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        if (name->text().trimmed().isEmpty() || command->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dlg, dlg.windowTitle(), tr("Name and command are required."));
            return;
        }
        dlg.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlg.resize(560, dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString dir = MimeApps::customActionsDir();
    QDir().mkpath(dir);
    QString slug = name->text().trimmed().toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    slug = slug.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (slug.isEmpty()) slug = QStringLiteral("action");
    QString leaf = QStringLiteral("rox-action-%1.desktop").arg(slug);
    for (int n = 2; QFileInfo::exists(dir + QLatin1Char('/') + leaf); ++n)
        leaf = QStringLiteral("rox-action-%1-%2.desktop").arg(slug).arg(n);

    QStringList targetList;
    if (tFiles->isChecked()) targetList << QStringLiteral("files");
    if (tDirs->isChecked()) targetList << QStringLiteral("directories");
    if (tLinks->isChecked()) targetList << QStringLiteral("links");

    auto oneLine = [](QString s) { return s.replace(QLatin1Char('\n'), QLatin1Char(' ')).trimmed(); };
    QSaveFile f(dir + QLatin1Char('/') + leaf);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(parent, dlg.windowTitle(), f.errorString());
        return;
    }
    QTextStream ts(&f);
    ts << "[Desktop Entry]\n"
       << "Type=Application\n"
       << "Name=" << oneLine(name->text()) << "\n"
       << "Exec=" << oneLine(command->text()) << "\n"
       << "Icon=" << oneLine(icon->text()) << "\n"
       << "Terminal=" << (terminal->isChecked() ? "true" : "false") << "\n"
       << "NoDisplay=true\n\n"
       << "[X-ROX-Filer Action]\n"
       << "MimeTypes=" << oneLine(mimes->text()) << "\n"
       << "Targets=" << targetList.join(QLatin1Char(';')) << ";\n"
       << "Selection=" << selection->currentData().toString() << "\n"
       << "Enabled=true\n";
    ts.flush();
    if (!f.commit())
        QMessageBox::warning(parent, dlg.windowTitle(), f.errorString());
}

/* ---------------------------------------------------------- Share folder */

void RoxDialogs::shareFolder(QWidget *parent, const QString &path, std::function<void(const QString &)> status)
{
    const QString net = QStandardPaths::findExecutable(QStringLiteral("net"));
    if (net.isEmpty()) {
        QMessageBox::warning(parent, tr("Share Folder"),
                             tr("The 'net' command was not found. Install Samba's command-line tools."));
        return;
    }
    QDialog dlg(parent);
    dlg.setWindowTitle(tr("Share Folder"));
    auto *layout = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    QString defName = QFileInfo(path).fileName();
    defName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
    auto *name = new QLineEdit(defName.left(80), &dlg);
    name->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z0-9_.-]{1,80}")), name));
    auto *comment = new QLineEdit(&dlg);
    auto *writable = new QCheckBox(tr("Allow others to modify the contents"), &dlg);
    auto *guest = new QCheckBox(tr("Allow guest access (no password)"), &dlg);
    form->addRow(tr("Folder:"), selectableLabel(path, &dlg));
    form->addRow(tr("Share name:"), name);
    form->addRow(tr("Comment:"), comment);
    form->addRow(QString(), writable);
    form->addRow(QString(), guest);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Share"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted || name->text().isEmpty())
        return;

    const QString acl = writable->isChecked()
        ? QStringLiteral("Everyone:F")
        : QStringLiteral("Everyone:R,%1:F").arg(userName());
    QStringList cmd{net, QStringLiteral("usershare"), QStringLiteral("add"), QStringLiteral("--long"),
                    name->text(), path, comment->text(), acl};
    if (guest->isChecked())
        cmd << QStringLiteral("guest_ok=y");
    const QString shareName = name->text();
    QPointer<QWidget> guard(parent);
    FileJob::run(parent, tr("Share Folder"), {cmd}, [guard, status, shareName](bool ok, const QString &errors) {
        if (!ok) {
            QMessageBox::warning(guard, tr("Share Folder"),
                                 errors.isEmpty() ? tr("The folder could not be shared.") : errors);
            return;
        }
        if (status)
            status(tr("Folder shared as “%1”.").arg(shareName));
    });
}

/* --------------------------------------------------------- Shell command */

void RoxDialogs::shellCommand(QWidget *parent, const QString &dir, const QStringList &selected,
                              std::function<void()> done)
{
    QSettings settings;
    bool ok = false;
    const QString command = QInputDialog::getText(parent, tr("Shell Command"),
        tr("Command to run in %1\n(selected items are passed as $1, $2…):").arg(dir),
        QLineEdit::Normal, settings.value(QStringLiteral("shell/last")).toString(), &ok).trimmed();
    if (!ok || command.isEmpty())
        return;
    settings.setValue(QStringLiteral("shell/last"), command);

    auto *p = new QProcess;
    p->setWorkingDirectory(dir);
    p->setProcessChannelMode(QProcess::MergedChannels);
    QPointer<QWidget> guard(parent);
    QObject::connect(p, &QProcess::finished, p, [p, guard, command, done](int code, QProcess::ExitStatus) {
        const QString out = QString::fromLocal8Bit(p->readAll()).trimmed();
        p->deleteLater();
        if (done) done();
        if (out.isEmpty() && code == 0)
            return;
        auto *dlg = new QDialog(guard);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowTitle(tr("Shell Command"));
        auto *layout = new QVBoxLayout(dlg);
        layout->addWidget(new QLabel(tr("“%1” exited with status %2").arg(command).arg(code), dlg));
        auto *text = new QPlainTextEdit(out, dlg);
        text->setReadOnly(true);
        text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        layout->addWidget(text);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
        layout->addWidget(buttons);
        dlg->resize(640, 400);
        dlg->show();
    });
    QStringList args{QStringLiteral("-c"), command, QStringLiteral("sh")};
    for (const QString &s : selected)
        args << QFileInfo(s).fileName();
    p->start(QStringLiteral("/bin/sh"), args);
}

/* ------------------------------------------------- extended attributes */

void RoxDialogs::extendedAttributes(QWidget *parent, const QString &path)
{
    QString error;
    const QList<Xattr::Attribute> original = Xattr::list(path, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(parent, tr("Extended attributes"),
                             tr("Could not read the extended attributes of “%1”:\n%2").arg(path, error));
        return;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(tr("Extended attributes — %1").arg(QFileInfo(path).fileName()));
    dialog.resize(600, 380);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(0, 2, &dialog);
    table->setHorizontalHeaderLabels({tr("Name"), tr("Value")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(table, 1);

    constexpr int kBinaryRole = Qt::UserRole + 1;
    constexpr int kExistingRole = Qt::UserRole + 2;
    auto addRow = [table](const QByteArray &name, const QByteArray &value, bool existing) {
        const int row = table->rowCount();
        table->insertRow(row);
        auto *nameItem = new QTableWidgetItem(QString::fromUtf8(name));
        nameItem->setData(kExistingRole, existing);
        if (existing)
            nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        const bool text = Xattr::isText(value);
        auto *valueItem = new QTableWidgetItem(text ? QString::fromUtf8(value)
            : RoxDialogs::tr("(binary, %n byte(s))", nullptr, int(value.size())));
        valueItem->setData(kBinaryRole, !text);
        valueItem->setData(Qt::UserRole, text ? QString::fromUtf8(value) : QString());
        if (!text) // binary values are shown, never rewritten
            valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 0, nameItem);
        table->setItem(row, 1, valueItem);
    };
    for (const Xattr::Attribute &attribute : original)
        addRow(attribute.name, attribute.value, true);

    auto *rowButtons = new QHBoxLayout;
    auto *add = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add"), &dialog);
    auto *remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), &dialog);
    rowButtons->addWidget(add);
    rowButtons->addWidget(remove);
    rowButtons->addStretch();
    layout->addLayout(rowButtons);
    auto *note = new QLabel(tr("Names need a namespace, for example user.comment. Ordinary users can only write user.* attributes."), &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);

    connect(add, &QPushButton::clicked, &dialog, [&] {
        addRow("user.", QByteArray(), false);
        table->setCurrentCell(table->rowCount() - 1, 0);
        table->editItem(table->item(table->rowCount() - 1, 0));
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        const int row = table->currentRow();
        if (row >= 0)
            table->removeRow(row);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QStringList failures;
    QSet<QByteArray> kept;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QByteArray name = table->item(row, 0)->text().trimmed().toUtf8();
        QTableWidgetItem *valueItem = table->item(row, 1);
        if (valueItem->data(kBinaryRole).toBool()) {
            kept.insert(name);
            continue;
        }
        if (name.isEmpty() || !name.contains('.')) {
            failures << tr("“%1” is not a valid attribute name (it needs a namespace such as user.).")
                           .arg(QString::fromUtf8(name));
            continue;
        }
        kept.insert(name);
        const QString before = valueItem->data(Qt::UserRole).toString();
        const bool existing = table->item(row, 0)->data(kExistingRole).toBool();
        if (existing && before == valueItem->text())
            continue; // unchanged
        QString setError;
        if (!Xattr::set(path, name, valueItem->text().toUtf8(), &setError))
            failures << QStringLiteral("%1: %2").arg(QString::fromUtf8(name), setError);
    }
    for (const Xattr::Attribute &attribute : original) {
        if (kept.contains(attribute.name))
            continue;
        QString removeError;
        if (!Xattr::remove(path, attribute.name, &removeError))
            failures << QStringLiteral("%1: %2").arg(QString::fromUtf8(attribute.name), removeError);
    }
    if (!failures.isEmpty())
        QMessageBox::warning(parent, tr("Extended attributes"), failures.join(QLatin1Char('\n')));
}
