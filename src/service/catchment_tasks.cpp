// Transient, bounded execution of full-grid catchment requests. All task
// results are spooled to disk, never retained as large QHash payloads.
#include <aowis/hydrology/service.h>

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSaveFile>
#include <QUuid>

#include <utility>

namespace Aowis::Hydrology
{
QJsonObject AnalysisService::catchmentTaskJson(const QSharedPointer<CatchmentTask> &task) const
{
    return QJsonObject{
        {QStringLiteral("task_id"), task->id},
        {QStringLiteral("job_id"), task->analysis_id},
        {QStringLiteral("x"), task->x},
        {QStringLiteral("y"), task->y},
        {QStringLiteral("state"), task->state},
        {QStringLiteral("error"), task->error},
        {QStringLiteral("result_ready"), task->state == QStringLiteral("completed")}
    };
}

// Caller holds catchment_tasks_mutex. Retain four terminal results at most,
// in addition to up to four accepted queued/running tasks. Discard oldest
// terminal task metadata and its spool file together.
void AnalysisService::pruneCatchmentResultsLocked()
{
    int terminal = 0;
    for (const QString &id : std::as_const(this->catchment_task_order))
    {
        const auto task = this->catchment_tasks.value(id);
        if (task && (task->state == QStringLiteral("completed") ||
                     task->state == QStringLiteral("failed") ||
                     task->state == QStringLiteral("canceled"))) ++terminal;
    }
    while (terminal > 4)
    {
        bool pruned = false;
        for (qsizetype i = 0; i < this->catchment_task_order.size(); ++i)
        {
            const QString id = this->catchment_task_order.at(i);
            const auto task = this->catchment_tasks.value(id);
            if (!task || (task->state != QStringLiteral("completed") &&
                          task->state != QStringLiteral("failed") &&
                          task->state != QStringLiteral("canceled"))) continue;
            if (!task->mask_path.isEmpty()) QFile::remove(task->mask_path);
            this->catchment_tasks.remove(id);
            this->catchment_task_order.removeAt(i);
            --terminal;
            pruned = true;
            break;
        }
        if (!pruned) break;
    }
}

QJsonObject AnalysisService::startCatchment(const QString &id, int x, int y, QString *error)
{
    if (error) error->clear();
    // Validate before reserving a worker slot. A running analysis is not a
    // valid source of immutable flow graph data.
    const QJsonObject item = job(id);
    if (item.isEmpty())
    {
        if (error) *error = QStringLiteral("Unknown job");
        return {};
    }
    QString unavailable;
    if (!completedJob(id, &unavailable)) // also waits for restored-artifact verification
    {
        if (error) *error = unavailable == QStringLiteral("Unknown or incomplete job")
            ? QStringLiteral("Analysis not completed") : unavailable;
        return {};
    }
    if (x < 0 || y < 0 || x >= item.value(QStringLiteral("columns")).toInt() ||
        y >= item.value(QStringLiteral("rows")).toInt())
    {
        if (error) *error = QStringLiteral("Invalid catchment outlet cell");
        return {};
    }
    const auto task = QSharedPointer<CatchmentTask>::create();
    task->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    task->analysis_id = id;
    task->x = x;
    task->y = y;
    QJsonObject answer;
    {
        QMutexLocker lock(&this->catchment_tasks_mutex);
        int outstanding = 0;
        for (const auto &entry : std::as_const(this->catchment_tasks))
            if (entry->state == QStringLiteral("queued") ||
                entry->state == QStringLiteral("running") ||
                entry->state == QStringLiteral("cancel_requested")) ++outstanding;
        if (outstanding >= 4)
        {
            if (error) *error = QStringLiteral("Catchment task queue full (4 active/queued tasks)");
            return {};
        }
        pruneCatchmentResultsLocked();
        this->catchment_tasks.insert(task->id, task);
        this->catchment_task_order.enqueue(task->id);
        this->catchment_queue.enqueue(task);
        answer = catchmentTaskJson(task);
    }
    scheduleCatchmentTasks();
    return answer;
}

void AnalysisService::scheduleCatchmentTasks()
{
    QMutexLocker lock(&this->catchment_tasks_mutex);
    while (this->catchment_worker_slots < 1 && !this->catchment_queue.isEmpty())
    {
        const auto task = this->catchment_queue.dequeue();
        if (task->cancelled.load()) continue;
        ++this->catchment_worker_slots;
        this->catchment_workers.start([this, task]()
        {
            runCatchmentTask(task);
            {
                QMutexLocker done(&this->catchment_tasks_mutex);
                --this->catchment_worker_slots;
            }
            scheduleCatchmentTasks();
        });
    }
}

void AnalysisService::runCatchmentTask(const QSharedPointer<CatchmentTask> &task)
{
    {
        QMutexLocker lock(&this->catchment_tasks_mutex);
        if (task->cancelled.load()) return;
        task->state = QStringLiteral("running");
    }
    QString error;
    QByteArray mask = catchmentImpl(task->analysis_id, task->x, task->y,
                                    &task->cancelled, &error);
    QString spool_file;
    if (error.isEmpty() && !task->cancelled.load())
    {
        spool_file = QDir(this->catchment_spool->path()).filePath(task->id + QStringLiteral(".u8"));
        QSaveFile file(spool_file);
        if (!file.open(QIODevice::WriteOnly) || file.write(mask) != mask.size() || !file.commit())
            error = QStringLiteral("Cannot persist transient catchment mask");
    }
    // The QByteArray is dropped before the next catchment computation, keeping
    // large completed masks out of the service's resident memory.
    mask.clear();
    {
        QMutexLocker lock(&this->catchment_tasks_mutex);
        if (!error.isEmpty())
        {
            task->state = task->cancelled.load() ? QStringLiteral("canceled") : QStringLiteral("failed");
            if (task->state == QStringLiteral("failed")) task->error = error;
        }
        else if (task->cancelled.load())
            task->state = QStringLiteral("canceled");
        else
        {
            task->state = QStringLiteral("completed");
            task->mask_path = spool_file;
        }
        if (task->state != QStringLiteral("completed") && !spool_file.isEmpty())
            QFile::remove(spool_file);
        pruneCatchmentResultsLocked();
    }
}

QJsonObject AnalysisService::catchmentTask(const QString &id,
    const QString &task_id, QString *error) const
{
    if (error) error->clear();
    QMutexLocker lock(&this->catchment_tasks_mutex);
    const auto task = this->catchment_tasks.value(task_id);
    if (!task || task->analysis_id != id)
    {
        if (error) *error = QStringLiteral("Unknown catchment task");
        return {};
    }
    return catchmentTaskJson(task);
}

std::shared_ptr<QFile> AnalysisService::catchmentTaskMaskFile(const QString &id,
    const QString &task_id, QString *error) const
{
    if (error) error->clear();
    // Eviction can unlink a spool file; acquire an open descriptor while
    // holding the task lock. Once opened, it remains readable after unlink
    // on supported Unix filesystems, even if the task metadata is evicted.
    QMutexLocker lock(&this->catchment_tasks_mutex);
    const auto task = this->catchment_tasks.value(task_id);
    if (!task || task->analysis_id != id)
    {
        if (error) *error = QStringLiteral("Unknown catchment task");
        return {};
    }
    if (task->state != QStringLiteral("completed"))
    {
        if (error) *error = QStringLiteral("Catchment result not ready");
        return {};
    }
    auto file = std::make_shared<QFile>(task->mask_path);
    if (!file->open(QIODevice::ReadOnly) || file->size() <= 0)
    {
        if (error) *error = QStringLiteral("Catchment result unavailable");
        return {};
    }
    return file;
}

QByteArray AnalysisService::catchmentTaskMask(const QString &id,
    const QString &task_id, QString *error) const
{
    const auto file = catchmentTaskMaskFile(id, task_id, error);
    if (!file) return {};
    // Retain the established in-process API. HTTP uses the file handle above
    // and never needs to materialize the whole mask in a QByteArray.
    const QByteArray mask = file->readAll();
    if (mask.size() != file->size())
    {
        if (error) *error = QStringLiteral("Catchment result unavailable");
        return {};
    }
    return mask;
}

bool AnalysisService::cancelCatchmentTask(const QString &id,
    const QString &task_id, QString *error)
{
    if (error) error->clear();
    QMutexLocker lock(&this->catchment_tasks_mutex);
    const auto task = this->catchment_tasks.value(task_id);
    if (!task || task->analysis_id != id)
    {
        if (error) *error = QStringLiteral("Unknown catchment task");
        return false;
    }
    if (task->state == QStringLiteral("queued"))
    {
        task->cancelled.store(true);
        task->state = QStringLiteral("canceled");
        this->catchment_queue.removeAll(task);
        pruneCatchmentResultsLocked();
    }
    else if (task->state == QStringLiteral("running"))
    {
        task->cancelled.store(true);
        task->state = QStringLiteral("cancel_requested");
    }
    return true;
}
} // namespace Aowis::Hydrology
