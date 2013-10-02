/*
 * Copyright 2013 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *      Martin Preisler <mpreisle@redhat.com>
 */

#include "OscapScannerBase.h"
#include "ScanningSession.h"

#include <QThread>
#include <QAbstractEventDispatcher>
#include <QTemporaryFile>
#include <cassert>

extern "C"
{
#include <xccdf_session.h>
}

OscapScannerBase::OscapScannerBase():
    Scanner(),

    mLastRuleID(""),
    mReadingRuleID(true),
    mReadBuffer(""),
    mCancelRequested(false)
{}

OscapScannerBase::~OscapScannerBase()
{}

void OscapScannerBase::cancel()
{
    // NB: No need for mutexes here, this will be run in the same thread because
    //     the event queue we pump in evaluate will run it.
    mCancelRequested = true;
}

void OscapScannerBase::getResults(QByteArray& destination)
{
    assert(!mCancelRequested);

    destination.append(mResults);
}

void OscapScannerBase::getReport(QByteArray& destination)
{
    assert(!mCancelRequested);

    destination.append(mReport);
}

void OscapScannerBase::getARF(QByteArray& destination)
{
    assert(!mCancelRequested);

    destination.append(mARF);
}

void OscapScannerBase::signalCompletion(bool canceled)
{
    Scanner::signalCompletion(canceled);

    mLastRuleID = "";
    mReadingRuleID = true;
    mReadBuffer = "";

    // reset the cancel flag now that we have finished XOR canceled
    mCancelRequested = false;
}

bool OscapScannerBase::checkPrerequisites()
{
    if (!mCapabilities.baselineSupport())
    {
        emit errorMessage(
            QString("oscap tool doesn't support basic features required for workbench. "
                "Please make sure you have openscap 0.8.0 or newer. "
                "oscap version was detected as '%1'.").arg(mCapabilities.getOpenSCAPVersion())
        );

        return false;
    }

    if (mScannerMode == SM_SCAN_ONLINE_REMEDIATION && !mCapabilities.onlineRemediation())
    {
        emit errorMessage(
            QString("oscap tool doesn't support online remediation. "
                "Please make sure you have openscap 0.9.5 or newer if you want "
                "to use online remediation. "
                "oscap version was detected as '%1'.").arg(mCapabilities.getOpenSCAPVersion())
        );

        return false;
    }

    if (mScannerMode == SM_OFFLINE_REMEDIATION && !mCapabilities.ARFInput())
    {
        emit errorMessage(
            QString("oscap tool doesn't support taking ARFs (result datastreams) as input. "
                "Please make sure you have openscap <NOT IMPLEMENTED YET> or newer if you want "
                "to use offline remediation. "
                "oscap version was detected as '%1'.").arg(mCapabilities.getOpenSCAPVersion())
        );

        return false;
    }

    if (mSession->isSDS() && !mCapabilities.sourceDatastreams())
    {
        emit errorMessage(
            QString("oscap tool doesn't support source datastreams as input. "
                "Please make sure you have openscap 0.9.0 or newer if you want "
                "to use source datastreams. "
                "oscap version was detected as '%1'.").arg(mCapabilities.getOpenSCAPVersion())
        );

        return false;
    }

    if (mSession->hasTailoring() && !mCapabilities.tailoringSupport())
    {
        emit errorMessage(
            QString("oscap tool doesn't support XCCDF tailoring but the session uses tailoring. "
                "Please make sure you have openscap 0.9.12 or newer on the target machine if you "
                 "want to use tailoring features of scap-workbench. "
                "oscap version was detected as '%1'.").arg(mCapabilities.getOpenSCAPVersion())
        );

        return false;
    }

    return true;
}

QStringList OscapScannerBase::buildEvaluationArgs(const QString& inputFile,
        const QString& tailoringFile,
        const QString& resultFile,
        const QString& reportFile,
        const QString& arfFile,
        bool onlineRemediation) const
{
    QStringList ret;
    ret.append("xccdf");
    ret.append("eval");

    if (mSession->isSDS())
    {
        const QString datastreamId = mSession->getDatastreamID();
        const QString componentId = mSession->getComponentID();

        if (!datastreamId.isEmpty())
        {
            ret.append("--datastream-id");
            ret.append(datastreamId);
        }

        if (!componentId.isEmpty())
        {
            ret.append("--xccdf-id");
            ret.append(componentId);
        }
    }

    if (!tailoringFile.isEmpty())
    {
        ret.append("--tailoring-file");
        ret.append(tailoringFile);
    }

    const QString profileId = mSession->getProfileID();

    if (!profileId.isEmpty())
    {
        ret.append("--profile");
        ret.append(profileId);
    }

    ret.append("--results");
    ret.append(resultFile);

    ret.append("--results-arf");
    ret.append(arfFile);

    ret.append("--report");
    ret.append(reportFile);

    if (mCapabilities.progressReporting())
        ret.append("--progress");

    if (onlineRemediation && mCapabilities.onlineRemediation())
        ret.append("--remediate");

    ret.append(inputFile);

    return ret;
}

QStringList OscapScannerBase::buildOfflineRemediationArgs(const QString& resultInputFile,
        const QString& resultFile,
        const QString& reportFile,
        const QString& arfFile) const
{
    QStringList ret;
    ret.append("xccdf");
    ret.append("remediate");

    ret.append("--results");
    ret.append(resultFile);

    ret.append("--results-arf");
    ret.append(arfFile);

    ret.append("--report");
    ret.append(reportFile);

    if (mCapabilities.progressReporting())
        ret.append("--progress");

    ret.append(resultInputFile);

    return ret;
}

bool OscapScannerBase::tryToReadStdOut(QProcess& process)
{
    process.setReadChannel(QProcess::StandardOutput);

    if (process.bytesAvailable() <= 0)
        return false;

    char buffer[1];
    buffer[0] = '\0';

    if (process.read(buffer, 1) != 1)
    {
        emit warningMessage(QString(
            "Error: Could not read from stdout of running 'oscap' process. "
            "This is very strange and most likely a bug. "
            "Read buffer is '%1'.").arg(mReadBuffer));
    }

    if (!mCapabilities.progressReporting())
        return true; // We did read something but it's not in a format we can parse.

    if (buffer[0] == ':')
    {
        mLastRuleID = mReadBuffer;
        if (mReadingRuleID) // sanity check
        {
            emit progressReport(mLastRuleID, "processing");
        }
        else
        {
            emit warningMessage(QString(
                "Error when parsing scan progress output from stdout of the 'oscap' process. "
                "':' encountered while not reading rule ID, newline and/or rule result are missing! "
                "Read buffer is '%1'.").arg(mReadBuffer));
        }
        mReadBuffer = "";
        mReadingRuleID = false;
    }
    else if (buffer[0] == '\n')
    {
        if (!mReadingRuleID) // sanity check
        {
            emit progressReport(mLastRuleID, mReadBuffer);
        }
        else
        {
            emit warningMessage(QString(
                "Error when parsing scan progress output from stdout of the 'oscap' process. "
                "Newline encountered while reading rule ID, rule result and/or ':' are missing! "
                "Read buffer is '%1'.").arg(mReadBuffer));
        }
        mReadBuffer = "";
        mReadingRuleID = true;
    }
    else
    {
        mReadBuffer += buffer[0];
    }

    return true;
}

void OscapScannerBase::watchStdErr(QProcess& process)
{
    process.setReadChannel(QProcess::StandardError);

    QString errorMessage = QString::Null();

    while (process.canReadLine())
    {
        // Trailing \n is returned by QProcess::readLine
        errorMessage += process.readLine();
    }

    if (!errorMessage.isEmpty())
    {
        emit warningMessage(QString("The 'oscap' process has written the following content to stderr:\n"
                                    "%1").arg(errorMessage));
    }
}