# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors

[CmdletBinding(DefaultParameterSetName = 'Hosted')]
param(
    [Parameter(Mandatory = $true)][long]$WindowHandle,
    [Parameter(Mandatory = $true)][int]$HostProcessId,
    [Parameter(Mandatory = $true, ParameterSetName = 'Hosted')][string]$BulkReadyEvent,
    [Parameter(Mandatory = $true, ParameterSetName = 'Calibration')][switch]$SmallEventsOnly
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase
$references = @('System.dll', 'System.Core.dll', [System.Windows.Automation.AutomationElement].Assembly.Location,
    [System.Windows.Automation.AutomationIdentifier].Assembly.Location, [System.Windows.Rect].Assembly.Location)
Add-Type -ReferencedAssemblies $references -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Windows.Automation;

public static class TrackSelectionEvents
{
    private static readonly AutomationEvent Selected = SelectionItemPattern.ElementSelectedEvent;
    private static readonly AutomationEvent Added = SelectionItemPattern.ElementAddedToSelectionEvent;
    private static readonly AutomationEvent Removed = SelectionItemPattern.ElementRemovedFromSelectionEvent;
    private static readonly AutomationEvent[] Subscriptions = { Selected, Added, Removed, SelectionPattern.InvalidatedEvent };

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static SelectionItemPattern Item(AutomationElement element)
    {
        return (SelectionItemPattern)element.GetCurrentPattern(SelectionItemPattern.Pattern);
    }

    private static AutomationElement Container(AutomationElement element)
    {
        object item;
        return element.TryGetCurrentPattern(SelectionItemPattern.Pattern, out item)
            ? ((SelectionItemPattern)item).Current.SelectionContainer : null;
    }

    private static string Identity(AutomationElement element)
    {
        return String.Join(".", element.GetRuntimeId());
    }

    private static bool SamePeer(AutomationElement actual, AutomationElement expected)
    {
        return actual != null && Identity(actual) == Identity(expected) && Automation.Compare(actual, expected);
    }

    private static Condition Property(AutomationProperty property, object value)
    {
        return new PropertyCondition(property, value);
    }

    private static string Describe(int eventId, string name, string runtimeId, int pid)
    {
        return String.Format("{0} from '{1}' ({2}, PID {3})", eventId, name, runtimeId, pid);
    }

    private sealed class Scenario
    {
        private readonly AutomationElement list;
        private readonly AutomationElement[] rows;
        private readonly string[] rowNames, rowIds;
        private readonly int pid;
        private readonly ConcurrentQueue<string> events = new ConcurrentQueue<string>();

        public Scenario(IntPtr hwnd, int hostPid)
        {
            pid = hostPid;
            var window = AutomationElement.FromHandle(hwnd);
            Require(window.Current.ProcessId == pid && pid != Process.GetCurrentProcess().Id,
                "expected the audit window in a different process");
            var table = window.FindFirst(TreeScope.Descendants,
                Property(AutomationElement.AutomationIdProperty, "modern-track-table"));
            Require(table != null, "shipping Modern track table is not exposed by UIA");
            var visible = table.FindAll(TreeScope.Descendants, new AndCondition(
                Property(AutomationElement.ControlTypeProperty, ControlType.ListItem),
                Property(AutomationElement.IsEnabledProperty, true),
                Property(AutomationElement.IsOffscreenProperty, false))).Cast<AutomationElement>()
                .Where(row => row.Current.ProcessId == pid && !row.Current.BoundingRectangle.IsEmpty &&
                    !String.IsNullOrWhiteSpace(row.Current.Name)).ToArray();
            list = visible.Select(Container).FirstOrDefault(container => container != null);
            Require(list != null && list.Current.ControlType == ControlType.List && list.Current.ProcessId == pid &&
                list.Current.IsEnabled && !list.Current.IsOffscreen && !list.Current.BoundingRectangle.IsEmpty,
                "expected the real on-screen track list as SelectionContainer");
            var peers = visible.Where(row => SamePeer(Container(row), list))
                .GroupBy(Identity).Select(group => group.First()).OrderBy(row => row.Current.BoundingRectangle.Top).ToArray();
            Require(peers.Length >= 3, "expected three distinct visible tracks with the same selection container");
            rows = peers.Length >= 5 ? new[] { peers[0], peers[2], peers[4] } : peers.Take(3).ToArray();
            rowNames = rows.Select(row => row.Current.Name).ToArray();
            rowIds = rows.Select(Identity).ToArray();
            var ancestor = TreeWalker.RawViewWalker.GetParent(list);
            while (ancestor != null && ancestor.Current.ProcessId == pid && !SamePeer(ancestor, table))
                ancestor = TreeWalker.RawViewWalker.GetParent(ancestor);
            Require(SamePeer(ancestor, table), "selection list is not inside the shipping track table");
        }

        private void Receive(object sender, AutomationEventArgs args)
        {
            // Use only the event's cache on the callback thread; no calls back into WinUI.
            try
            {
                var element = (AutomationElement)sender;
                events.Enqueue(Describe(args.EventId.Id,
                    // An unnamed list may report an unsupported Name. Its RuntimeId and PID remain mandatory;
                    // a missing name on an item still fails against that track's nonempty name.
                    element.GetCachedPropertyValue(AutomationElement.NameProperty, true) as string ?? String.Empty,
                    String.Join(".", (int[])element.GetCachedPropertyValue(AutomationElement.RuntimeIdProperty, true)),
                    (int)element.GetCachedPropertyValue(AutomationElement.ProcessIdProperty, true)));
            }
            catch (Exception error) { events.Enqueue("unreadable callback: " + error); }
        }

        private void AssertSelection(string stage, int[] selected, int? total = null)
        {
            for (var i = 0; i < rows.Length; ++i)
            {
                var item = Item(rows[i]).Current;
                Require(Identity(rows[i]) == rowIds[i] && SamePeer(item.SelectionContainer, list) &&
                    item.IsSelected == selected.Contains(i),
                    stage + ": wrong row selection or SelectionContainer for " + rowNames[i]);
            }
            var info = ((SelectionPattern)list.GetCurrentPattern(SelectionPattern.Pattern)).Current;
            var actual = info.GetSelection();
            Require(info.CanSelectMultiple && !info.IsSelectionRequired && actual != null &&
                actual.Length == (total ?? selected.Length) && actual.All(peer => peer != null) &&
                actual.Select(Identity).Distinct().Count() == actual.Length &&
                selected.All(i => actual.Any(peer => SamePeer(peer, rows[i]))),
                stage + ": GetSelection did not return the expected provider identities/count; observed " +
                    (actual == null ? "null" : actual.Length.ToString()));
        }

        private string Event(AutomationEvent type, int row)
        {
            return Describe(type.Id, rowNames[row], rowIds[row], pid);
        }

        private void Observe(string stage, Action action, params string[] expected)
        {
            action();
            var elapsed = Stopwatch.StartNew();
            // The callback can enter before or after the action returns. Keep one stream across steps,
            // and leave a bounded quiet interval to catch wrong/duplicate events, including on a no-op.
            while (events.Count < expected.Length && elapsed.ElapsedMilliseconds < 4000)
                Thread.Sleep(10);
            Thread.Sleep(expected.Length == 0 ? 1000 : 250);
            var observed = events.ToArray();
            Console.WriteLine("{0}: {1} ({2} ms observation after call)", stage,
                String.Join("; ", observed), elapsed.ElapsedMilliseconds);
            Require(observed.OrderBy(item => item).SequenceEqual(expected.OrderBy(item => item)),
                stage + ": expected " + String.Join("; ", expected) + "; observed " + String.Join("; ", observed));
            // Drain exactly the entries sampled; a later callback remains visible to the next assertion.
            foreach (var entry in observed)
            {
                string removed;
                Require(events.TryDequeue(out removed) && removed == entry, stage + ": callback stream lost an entry");
            }
        }

        private void Step(string stage, Action action, int[] selected, params string[] expected)
        {
            Observe(stage, action, expected);
            AssertSelection(stage, selected);
        }

        public void Run(string bulkReadyEvent)
        {
            AssertSelection("empty setup", new int[0]);
            Item(rows[0]).Select();
            AssertSelection("selected before subscribing", new[] { 0 });
            var cache = new CacheRequest { TreeScope = TreeScope.Element, TreeFilter = Automation.RawViewCondition,
                AutomationElementMode = AutomationElementMode.None };
            cache.Add(AutomationElement.NameProperty);
            cache.Add(AutomationElement.RuntimeIdProperty);
            cache.Add(AutomationElement.ProcessIdProperty);
            AutomationEventHandler handler = Receive;
            var registered = 0;
            try
            {
                using (cache.Activate())
                {
                    foreach (var type in Subscriptions)
                    {
                        Automation.AddAutomationEventHandler(type, list, TreeScope.Subtree, handler);
                        ++registered;
                    }
                }
                Console.WriteLine("Out-of-process UIA: list {0} in PID {1}", Identity(list), pid);
                Observe("quiet nonempty baseline", () => { });
                Step("Add B after subscribing to selected A", () => Item(rows[1]).AddToSelection(),
                    new[] { 0, 1 }, Event(Added, 1));
                Step("Add C", () => Item(rows[2]).AddToSelection(), new[] { 0, 1, 2 }, Event(Added, 2));
                Step("Remove C (3 to 2)", () => Item(rows[2]).RemoveFromSelection(), new[] { 0, 1 }, Event(Removed, 2));
                Step("Remove B (keep sole A)", () => Item(rows[1]).RemoveFromSelection(), new[] { 0 }, Event(Removed, 1));
                Step("Select B (replace A)", () => Item(rows[1]).Select(), new[] { 1 }, Event(Selected, 1), Event(Removed, 0));
                Step("Select already-sole B (no-op)", () => Item(rows[1]).Select(), new[] { 1 });
                Step("Clear B", () => Item(rows[1]).RemoveFromSelection(), new int[0], Event(Removed, 1));
                Step("Select A from empty", () => Item(rows[0]).Select(), new[] { 0 }, Event(Selected, 0));
                Step("Clear A", () => Item(rows[0]).RemoveFromSelection(), new int[0], Event(Removed, 0));

                if (!String.IsNullOrEmpty(bulkReadyEvent))
                {
                    // SelectionItem has no bulk call. Let the host apply a single native range only once armed.
                    using (var ready = EventWaitHandle.OpenExisting(bulkReadyEvent))
                    {
                        Observe("SelectRange (24 tracks)", () => ready.Set(),
                            Describe(SelectionPattern.InvalidatedEvent.Id, list.Current.Name, Identity(list), pid));
                    }
                    // Check count and distinct provider IDs, plus exact matches for the three realized witnesses.
                    AssertSelection("bulk selection", new[] { 0, 1, 2 }, 24);
                }
            }
            finally
            {
                for (var i = registered - 1; i >= 0; --i)
                    Automation.RemoveAutomationEventHandler(Subscriptions[i], list, handler);
            }
            Require(events.IsEmpty, "unexpected late callback(s): " + String.Join("; ", events));
        }
    }

    public static void Run(IntPtr hwnd, int pid, string bulkReadyEvent)
    {
        Require(Thread.CurrentThread.GetApartmentState() == ApartmentState.MTA, "UIA client must run in an MTA");
        new Scenario(hwnd, pid).Run(bulkReadyEvent);
    }
}
'@
if ($SmallEventsOnly) { Write-Host 'Small-event calibration only; bulk not exercised.' }
[TrackSelectionEvents]::Run([IntPtr]$WindowHandle, $HostProcessId, $BulkReadyEvent)
